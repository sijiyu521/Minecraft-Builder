/*
 * llm.c — talking to an OpenAI compatible /chat/completions endpoint.
 *
 * The important design rule of this project lives here: the model is asked for a
 * *plan* (a template id, palette slot ids, a few integers, some labels), never
 * for geometry. Everything it returns is run through normalize_plan(), which
 * validates every id against the real catalog and clamps every number, so a
 * hallucinating model can at worst pick the wrong template — it can never
 * produce a structure that cannot be built or that references a block which
 * does not exist.
 *
 * The prompt string is kept byte-for-byte stable on purpose: the golden tests
 * pin the exact response mapping, so any change to the wording would show up as
 * a behavioural difference against the recorded fixtures.
 */
#include "mb.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* presets                                                                    */
/* ========================================================================== */

const MBLlmPreset MB_LLM_PRESETS[] = {
    { "deepseek",  "DeepSeek",        "https://api.deepseek.com/v1",
      "deepseek-chat",   "官方直连，性价比高，中文表现好。", false },
    { "openai",    "OpenAI",          "https://api.openai.com/v1",
      "gpt-4o-mini",     "需要能访问 api.openai.com。", false },
    { "moonshot",  "Moonshot 月之暗面", "https://api.moonshot.cn/v1",
      "moonshot-v1-8k",  "国内可直连，长上下文。", false },
    { "zhipu",     "智谱 GLM",         "https://open.bigmodel.cn/api/paas/v4",
      "glm-4-flash",     "flash 系列免费额度较多。", false },
    { "dashscope", "通义千问 DashScope", "https://dashscope.aliyuncs.com/compatible-mode/v1",
      "qwen-plus",       "阿里云百炼的 OpenAI 兼容模式。", false },
    { "ollama",    "Ollama 本地",      "http://localhost:11434/v1",
      "qwen2.5:7b",      "本机模型，无需 API Key，完全离线。", true },
    { "custom",    "自定义 / 中转",     "", "",
      "任何兼容 /chat/completions 的服务，包括自建中转。", false },
};

const int MB_LLM_PRESET_COUNT = (int)(sizeof(MB_LLM_PRESETS) / sizeof(MB_LLM_PRESETS[0]));

void mb_llm_default_config(MBLlmConfig* cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->baseUrl, sizeof(cfg->baseUrl), "%s", MB_LLM_PRESETS[0].baseUrl);
    snprintf(cfg->model, sizeof(cfg->model), "%s", MB_LLM_PRESETS[0].model);
    cfg->apiKey[0] = '\0';
    cfg->temperature = 0.7;
    cfg->jsonMode = true;
}

/* ========================================================================== */
/* errors                                                                     */
/* ========================================================================== */

void mb_llm_error_set(MBLlmError* e, MBLlmErrorKind kind, const char* fmt, ...)
{
    if (!e) return;
    e->kind = kind;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->message, sizeof(e->message), fmt, ap);
    va_end(ap);
}

static const char* err_kind_name(MBLlmErrorKind k)
{
    switch (k) {
    case MB_LLM_ERR_NETWORK:      return "network";
    case MB_LLM_ERR_AUTH:         return "auth";
    case MB_LLM_ERR_RATE_LIMIT:   return "rate-limit";
    case MB_LLM_ERR_BAD_RESPONSE: return "bad-response";
    case MB_LLM_ERR_ABORTED:      return "aborted";
    case MB_LLM_ERR_UNKNOWN:      return "unknown";
    default:                      return "none";
    }
}

const char* mb_llm_error_kind_name(MBLlmErrorKind k) { return err_kind_name(k); }

/* ========================================================================== */
/* catalog + prompt                                                           */
/* ========================================================================== */

static void catalogue_append(SBuf* b)
{
    for (int i = 0; i < MB_MATERIAL_COUNT; i++) {
        const char* id = MB_MATERIALS[i].id;
        if (i) mb_sb_puts(b, "、");
        mb_sb_printf(b, "%s=%s", id + 10 /* "minecraft:" */, MB_MATERIALS[i].name);
    }
}

static char* system_prompt(Arena* a)
{
    SBuf b;
    mb_sb_init(&b, a);

    mb_sb_puts(&b,
        "你是一个 Minecraft 建筑规划助手。用户会用自然语言描述想要的建筑，"
        "你负责把它翻译成一份结构化的建筑意图（JSON）。\n\n");

    mb_sb_puts(&b, "## 可选的建筑类型（structure 只能填 id）\n");
    for (int i = 0; i < MB_STRUCTURE_COUNT; i++) {
        if (i) mb_sb_puts(&b, "\n");
        mb_sb_printf(&b, "- %s：%s —— %s",
                     MB_STRUCTURES[i].id, MB_STRUCTURES[i].label, MB_STRUCTURES[i].hint);
    }

    mb_sb_puts(&b, "\n\n## 可选的材质风格（style 只能填 id）\n");
    for (int i = 0; i < MB_STYLE_COUNT; i++) {
        if (i) mb_sb_puts(&b, "\n");
        mb_sb_printf(&b, "- %s：%s", MB_STYLES[i].id, MB_STYLES[i].label);
    }

    mb_sb_puts(&b,
        "\n\n## 可选的方块（palette 里只能填这些名字，不带 minecraft: 前缀）\n");
    catalogue_append(&b);

    mb_sb_puts(&b, "\n\n## 输出格式\n");
    mb_sb_puts(&b,
        "只输出一个 JSON 对象，不要有任何解释文字、不要用 markdown 代码块。字段如下：\n\n"
        "{\n"
        "  \"structure\": \"建筑类型 id\",\n"
        "  \"style\": \"材质风格 id\",\n"
        "  \"scale\": \"small | medium | large\",\n"
        "  \"width\": 建筑宽度(整数, 5-48),\n"
        "  \"depth\": 建筑进深(整数, 3-48),\n"
        "  \"height\": 建筑高度(整数, 3-40),\n"
        "  \"floors\": 楼层数(整数, 1-4),\n"
        "  \"palette\": {\n"
        "    \"wall\": \"主墙体方块\",\n"
        "    \"accent\": \"装饰/边框方块\",\n"
        "    \"roof\": \"屋顶方块\",\n"
        "    \"floor\": \"地板方块\",\n"
        "    \"frame\": \"结构梁柱方块\",\n"
        "    \"glass\": \"窗户方块\",\n"
        "    \"light\": \"照明方块\",\n"
        "    \"nature\": \"植被方块\"\n"
        "  },\n"
        "  \"name\": \"给这座建筑起的中文名字(不超过 8 字)\",\n"
        "  \"keywords\": [\"从用户描述里提取的关键词\"],\n"
        "  \"modifiers\": [\"用户要求的结构特征，如 两层结构 / 大面积采光 / 带烟囱\"],\n"
        "  \"notes\": [\"你对这个设计的一句中文说明，最多 3 条\"]\n"
        "}\n\n"
        "## 规则\n"
        "1. 用户明确给出的尺寸必须严格采用（\"15x11\" 表示 width=15、depth=11；"
        "\"高 24\" 表示 height=24；\"两层\"表示 floors=2）。\n"
        "2. 用户没给的尺寸，按建筑类型和体量合理估计，不要照抄示例。\n"
        "3. palette 必须与你选定的 style 相符，并且是上面列表里真实存在的方块名。"
        "用户点名了某个方块（如\"白桦木板\"）就用它。\n"
        "4. 一个字段都不能少，即使不确定也要给出合理默认值。\n"
        "5. 只输出 JSON。");

    return mb_sb_done(&b);
}

/* ========================================================================== */
/* response parsing                                                           */
/* ========================================================================== */

/* Repair trailing commas: /,\s*([}\]])/g -> '$1' */
static char* repair_trailing_commas(Arena* a, const char* s, size_t n)
{
    char* out = (char*)mb_arena_alloc(a, n + 1);
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == ',') {
            size_t j = i + 1;
            while (j < n && (s[j] == ' ' || s[j] == '\t' || s[j] == '\n' || s[j] == '\r' ||
                             s[j] == '\f' || s[j] == '\v')) {
                j++;
            }
            if (j < n && (s[j] == '}' || s[j] == ']')) {
                /* drop the comma and the whitespace we just skipped */
                i = j - 1;
                continue;
            }
        }
        out[w++] = s[i];
    }
    out[w] = '\0';
    return out;
}

MBJson* mb_extract_json_object(Arena* a, const char* text, MBLlmError* err)
{
    if (!text) {
        mb_llm_error_set(err, MB_LLM_ERR_BAD_RESPONSE, "模型没有返回 JSON 对象");
        return NULL;
    }

    size_t n = strlen(text);
    long start = -1;
    for (size_t i = 0; i < n; i++) {
        if (text[i] == '{') { start = (long)i; break; }
    }
    if (start < 0) {
        mb_llm_error_set(err, MB_LLM_ERR_BAD_RESPONSE, "模型没有返回 JSON 对象");
        return NULL;
    }

    int depth = 0;
    bool inString = false;
    bool escaped = false;

    for (size_t i = (size_t)start; i < n; i++) {
        char ch = text[i];
        if (escaped) { escaped = false; continue; }
        if (ch == '\\') { escaped = true; continue; }
        if (ch == '"') { inString = !inString; continue; }
        if (inString) continue;
        if (ch == '{') {
            depth++;
        } else if (ch == '}') {
            depth--;
            if (depth == 0) {
                size_t len = i - (size_t)start + 1;
                MBJson* v = mb_json_parse(a, text + start, len);
                if (v) return v;
                char* repaired = repair_trailing_commas(a, text + start, len);
                v = mb_json_parse(a, repaired, strlen(repaired));
                if (v) return v;
                mb_llm_error_set(err, MB_LLM_ERR_BAD_RESPONSE,
                                 "模型返回的 JSON 解析失败：无法解析 JSON 内容");
                return NULL;
            }
        }
    }

    mb_llm_error_set(err, MB_LLM_ERR_BAD_RESPONSE, "模型返回的 JSON 不完整");
    return NULL;
}

/* ========================================================================== */
/* plan validation                                                            */
/* ========================================================================== */

/*
 * Number(value) for the coercion rules in toInt(). Returns NAN for the cases
 * where JavaScript would produce NaN (which then falls back to the default).
 */
static double js_number_of(const MBJson* v)
{
    if (!v) return NAN;
    switch (v->type) {
    case MB_JSON_NUM:  return v->u.num;
    case MB_JSON_BOOL: return v->u.boolean ? 1.0 : 0.0;
    case MB_JSON_NULL: return 0.0;
    case MB_JSON_STR: {
        const char* s = v->u.str.ptr;
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f' || *s == '\v') s++;
        if (*s == '\0') return 0.0;
        if (strncmp(s, "Infinity", 8) == 0) return INFINITY;
        if (strncmp(s, "-Infinity", 9) == 0) return -INFINITY;
        char* end = NULL;
        double d = strtod(s, &end);
        if (end == s) return NAN;
        while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r' || *end == '\f' ||
               *end == '\v') end++;
        if (*end != '\0') return NAN;
        return d;
    }
    case MB_JSON_ARR:
        if (v->u.arr.count == 0) return 0.0;
        if (v->u.arr.count == 1) return js_number_of(v->u.arr.items[0]);
        return NAN;
    default:
        return NAN;
    }
}

static int to_int(const MBJson* v, int fallback, int min, int max)
{
    double n = js_number_of(v);
    if (!isfinite(n)) return fallback;
    int r = mb_js_round(n);
    return r < min ? min : (r > max ? max : r);
}

static bool json_is_nullish(const MBJson* v)
{
    return v == NULL || v->type == MB_JSON_NULL;
}

/* trim + UTF-16 slice(0, limit), mirroring String.prototype.slice. */
static char* trim_slice_utf16(Arena* a, const char* s, int limit)
{
    size_t n = 0;
    U16* u = mb_u16_from_utf8(a, s, &n);

    size_t start = 0, end = n;
    while (start < end && mb_u16_is_space(u[start])) start++;
    while (end > start && mb_u16_is_space(u[end - 1])) end--;

    size_t len = end - start;
    if (limit >= 0 && len > (size_t)limit) len = (size_t)limit;
    if (len == 0) return mb_arena_strdup(a, "");
    return mb_utf8_from_u16(a, u + start, len);
}

static const char** to_string_array(Arena* a, const MBJson* v, int limit, int* outCount)
{
    *outCount = 0;
    const char** out = (const char**)mb_arena_alloc(a, (size_t)(limit > 0 ? limit : 1) *
                                                          sizeof(char*));
    if (!v || v->type != MB_JSON_ARR) return out;

    for (size_t i = 0; i < v->u.arr.count && *outCount < limit; i++) {
        const MBJson* item = v->u.arr.items[i];
        if (!item || item->type != MB_JSON_STR) continue;
        char* t = trim_slice_utf16(a, item->u.str.ptr, -1);
        if (t[0] == '\0') continue;
        out[(*outCount)++] = t;
    }
    return out;
}

/* Accept both `stone_bricks` and `minecraft:stone_bricks`, drop junk. */
static const char* normalize_material(Arena* a, const MBJson* v)
{
    if (!v || v->type != MB_JSON_STR) return NULL;
    const char* raw = v->u.str.ptr;
    while (*raw == ' ' || *raw == '\t' || *raw == '\n' || *raw == '\r') raw++;

    /* strip a leading "minecraft:" case-insensitively */
    if (strlen(raw) >= 10 && mb_strncmp_ci(raw, "minecraft:", 10) == 0) raw += 10;

    /* trailing whitespace does not survive the set lookup */
    char buf[128];
    size_t w = 0;
    while (raw[w] && w + 1 < sizeof(buf)) { buf[w] = raw[w]; w++; }
    buf[w] = '\0';
    while (w > 0 && (buf[w - 1] == ' ' || buf[w - 1] == '\t' || buf[w - 1] == '\n' ||
                     buf[w - 1] == '\r')) {
        buf[--w] = '\0';
    }
    if (w == 0) return NULL;
    if (!mb_material_known(buf)) return NULL;
    return mb_arena_printf(a, "minecraft:%s", buf);
}

MBLlmPlan mb_normalize_plan(Arena* a, const MBJson* raw, MBLlmError* err)
{
    MBLlmPlan p;
    memset(&p, 0, sizeof(p));

    if (!raw || raw->type != MB_JSON_OBJ) {
        mb_llm_error_set(err, MB_LLM_ERR_BAD_RESPONSE, "模型返回的不是一个对象");
        return p;
    }

    /* --- structure / style: unknown ids fall back to the first catalog entry - */
    {
        char* s = mb_json_to_js_string(a, mb_json_get(raw, "structure"));
        if (mb_structure_by_id(s)) {
            p.structure = mb_structure_by_id(s)->id;
        } else {
            p.structure = MB_STRUCTURES[0].id;
        }
    }
    {
        char* s = mb_json_to_js_string(a, mb_json_get(raw, "style"));
        if (mb_style_by_id(s)) {
            p.style = mb_style_by_id(s)->id;
        } else {
            p.style = MB_STYLES[0].id;
        }
    }

    /* --- scale: only "small" and "large" survive, everything else is medium -- */
    {
        const MBJson* sv = mb_json_get(raw, "scale");
        char* s;
        if (json_is_nullish(sv)) {
            s = mb_arena_strdup(a, "");
        } else {
            s = mb_json_to_js_string(a, sv);
        }
        for (char* q = s; *q; q++) if (*q >= 'A' && *q <= 'Z') *q = (char)(*q - 'A' + 'a');
        if (strcmp(s, "small") == 0)      p.scale = "small";
        else if (strcmp(s, "large") == 0) p.scale = "large";
        else                              p.scale = "medium";
    }

    /* --- palette ------------------------------------------------------------ */
    {
        const MBJson* pal = mb_json_get(raw, "palette");
        if (pal && pal->type == MB_JSON_OBJ) {
            static const char* keys[8] = { "wall", "accent", "roof", "floor",
                                           "frame", "glass", "light", "nature" };
            const char** slots[8] = { &p.palette.wall,   &p.palette.accent,
                                      &p.palette.roof,   &p.palette.floor,
                                      &p.palette.frame,  &p.palette.glass,
                                      &p.palette.light,  &p.palette.nature };
            for (int i = 0; i < 8; i++) {
                *slots[i] = normalize_material(a, mb_json_get(pal, keys[i]));
            }
        }
    }

    /* --- labels + numbers --------------------------------------------------- */
    {
        const MBJson* nv = mb_json_get(raw, "name");
        p.name = (nv && nv->type == MB_JSON_STR) ? trim_slice_utf16(a, nv->u.str.ptr, 12)
                                                 : mb_arena_strdup(a, "");
    }

    p.width  = to_int(mb_json_get(raw, "width"),  15, 5, 48);
    p.depth  = to_int(mb_json_get(raw, "depth"),  11, 3, 48);
    p.height = to_int(mb_json_get(raw, "height"),  7, 3, 40);
    p.floors = to_int(mb_json_get(raw, "floors"),  1, 1, 4);

    p.keywords  = to_string_array(a, mb_json_get(raw, "keywords"), 8, &p.keywordCount);
    p.modifiers = to_string_array(a, mb_json_get(raw, "modifiers"), 6, &p.modifierCount);
    p.notes     = to_string_array(a, mb_json_get(raw, "notes"), 4, &p.noteCount);

    return p;
}

/* ========================================================================== */
/* transport                                                                  */
/* ========================================================================== */

char* mb_url_join(Arena* a, const char* baseUrl, const char* path)
{
    size_t len = strlen(baseUrl);
    while (len > 0 && baseUrl[len - 1] == '/') len--;
    return mb_arena_printf(a, "%.*s%s%s", (int)len, baseUrl,
                           path[0] == '/' ? "" : "/", path);
}

char* mb_url_host(Arena* a, const char* url)
{
    const char* p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char* end = p;
    while (*end && *end != '/' && *end != ':' && *end != '?' && *end != '#') end++;
    if (end == p) return mb_arena_strdup(a, url);
    return mb_arena_strndup(a, p, (size_t)(end - p));
}

static MBLlmErrorKind describe_http_error(long status, const char* body, char* message,
                                          size_t messageCap)
{
    char detail[512];
    snprintf(detail, sizeof(detail), "%.300s", body ? body : "");

    {
        const MBJson* parsed = NULL;
        Arena tmp;
        mb_arena_init(&tmp);
        if (body) parsed = mb_json_parse(&tmp, body, strlen(body));
        if (parsed) {
            const MBJson* errobj = mb_json_get(parsed, "error");
            const MBJson* msg = errobj ? mb_json_get(errobj, "message") : NULL;
            if (msg && msg->type == MB_JSON_STR) {
                snprintf(detail, sizeof(detail), "%.400s", msg->u.str.ptr);
            }
        }
        mb_arena_reset(&tmp);
    }

    if (status == 401 || status == 403) {
        snprintf(message, messageCap,
                 "API Key 无效或没有权限（HTTP %ld）：%s", status, detail);
        return MB_LLM_ERR_AUTH;
    }
    if (status == 429) {
        snprintf(message, messageCap,
                 "触发限流或额度用尽（HTTP 429）：%s", detail);
        return MB_LLM_ERR_RATE_LIMIT;
    }
    if (status == 404) {
        snprintf(message, messageCap,
                 "接口 404：base URL 或模型名不对（HTTP 404）：%s", detail);
        return MB_LLM_ERR_BAD_RESPONSE;
    }
    snprintf(message, messageCap, "请求失败（HTTP %ld）：%s", status, detail);
    return MB_LLM_ERR_UNKNOWN;
}

/*
 * One chat completion call. `jsonMode` toggles response_format, which not every
 * OpenAI compatible gateway implements — the caller retries without it.
 */
static bool post_chat(Arena* a, const MBLlmConfig* cfg, const char* systemText,
                      const char* userText, bool jsonMode, int maxTokens,
                      char** outContent, MBLlmError* err)
{
    /* --- build the request body ------------------------------------------- */
    SBuf body;
    mb_sb_init(&body, a);
    mb_sb_puts(&body, "{\"model\":");
    mb_json_escape(&body, cfg->model);
    mb_sb_puts(&body, ",\"messages\":[{\"role\":\"system\",\"content\":");
    mb_json_escape(&body, systemText);
    mb_sb_puts(&body, "},{\"role\":\"user\",\"content\":");
    mb_json_escape(&body, userText);
    mb_sb_puts(&body, "}],\"temperature\":");
    mb_sb_printf(&body, "%g", cfg->temperature);
    if (maxTokens > 0) mb_sb_printf(&body, ",\"max_tokens\":%d", maxTokens);
    if (jsonMode) mb_sb_puts(&body, ",\"response_format\":{\"type\":\"json_object\"}");
    mb_sb_puts(&body, "}");

    char* payload = mb_sb_done(&body);

    /* --- send ------------------------------------------------------------- */
    char auth[300];
    auth[0] = '\0';
    const char* key = cfg->apiKey;
    while (*key == ' ' || *key == '\t' || *key == '\n' || *key == '\r') key++;
    if (*key) {
        size_t klen = strlen(key);
        while (klen > 0 && (key[klen - 1] == ' ' || key[klen - 1] == '\t' ||
                            key[klen - 1] == '\n' || key[klen - 1] == '\r')) klen--;
        snprintf(auth, sizeof(auth), "Bearer %.*s", (int)klen, key);
    }

    char* url = mb_url_join(a, cfg->baseUrl, "/chat/completions");

    MBHttpRequest req;
    memset(&req, 0, sizeof(req));
    req.body = payload;
    req.bodyLen = strlen(payload);
    req.authorization = auth[0] ? auth : NULL;
    req.contentType = "application/json";

    MBHttpResponse res;
    memset(&res, 0, sizeof(res));
    res.error = err;

    if (!mb_http_do(a, "POST", url, &req, &res)) {
        if (err->kind == MB_LLM_ERR_NONE) {
            mb_llm_error_set(err, MB_LLM_ERR_NETWORK,
                "无法连接到模型服务。常见原因：base URL 写错、服务端不可达、"
                "或本机网络不通。");
        }
        return false;
    }

    if (res.status < 200 || res.status >= 300) {
        char msg[1024];
        MBLlmErrorKind kind = describe_http_error(res.status, res.body, msg, sizeof(msg));
        mb_llm_error_set(err, kind, "%s", msg);
        return false;
    }

    /* --- parse the envelope ----------------------------------------------- */
    MBJson* parsed = mb_json_parse(a, res.body ? res.body : "", res.bodyLen);
    if (!parsed) {
        mb_llm_error_set(err, MB_LLM_ERR_BAD_RESPONSE, "服务返回的不是 JSON：%.200s",
                         res.body ? res.body : "");
        return false;
    }

    const MBJson* choices = mb_json_get(parsed, "choices");
    const MBJson* first = mb_json_at(choices, 0);
    const MBJson* message = first ? mb_json_get(first, "message") : NULL;
    const MBJson* content = message ? mb_json_get(message, "content") : NULL;
    if (!content || content->type != MB_JSON_STR || content->u.str.len == 0) {
        mb_llm_error_set(err, MB_LLM_ERR_BAD_RESPONSE, "模型返回了空内容");
        return false;
    }

    *outContent = content->u.str.ptr;
    return true;
}

/* /response_format|json_object|unsupported|invalid.*param/i, same line only. */
static bool looks_like_response_format_rejection(const char* message)
{
    static const char* needles[] = { "response_format", "json_object", "unsupported" };
    for (int i = 0; i < 3; i++) {
        if (mb_contains_ci(message, needles[i])) return true;
    }
    /* invalid.*param — `.` does not cross line boundaries in JavaScript */
    const char* p = message;
    while ((p = mb_strstr_ci(p, "invalid")) != NULL) {
        const char* q = p + 7;
        while (*q && *q != '\n' && *q != '\r') {
            if (mb_strncmp_ci(q, "param", 5) == 0) return true;
            q++;
        }
        p += 7;
    }
    return false;
}

/* ========================================================================== */
/* public API                                                                 */
/* ========================================================================== */

MBLlmPlanOutcome mb_llm_plan(Arena* a, const char* prompt, const MBLlmConfig* cfg,
                             const char* lockStructure, const char* lockStyle)
{
    MBLlmPlanOutcome out;
    memset(&out, 0, sizeof(out));

    if (!cfg->baseUrl[0]) {
        mb_llm_error_set(&out.error, MB_LLM_ERR_UNKNOWN, "请先填写 base URL");
        return out;
    }
    if (!cfg->model[0]) {
        mb_llm_error_set(&out.error, MB_LLM_ERR_UNKNOWN, "请先填写模型名");
        return out;
    }

    /* --- the user turn, with any hard UI locks appended ------------------- */
    char* userText;
    if (lockStructure || lockStyle) {
        SBuf ub;
        mb_sb_init(&ub, a);
        mb_sb_puts(&ub, prompt);
        mb_sb_puts(&ub, "\n\n【额外约束】");
        if (lockStructure) mb_sb_printf(&ub, "建筑类型固定为 %s，不要改。", lockStructure);
        if (lockStyle)     mb_sb_printf(&ub, "材质风格固定为 %s，不要改。", lockStyle);
        userText = mb_sb_done(&ub);
    } else {
        userText = mb_arena_strdup(a, prompt);
    }

    char* systemText = system_prompt(a);

    /* --- first attempt ---------------------------------------------------- */
    double started = mb_now_ms();
    char* content = NULL;
    if (!post_chat(a, cfg, systemText, userText, cfg->jsonMode, 0, &content, &out.error)) {
        /* Some gateways 400 on response_format. Retry once without it. */
        bool canRetry = cfg->jsonMode && out.error.kind == MB_LLM_ERR_UNKNOWN &&
                        looks_like_response_format_rejection(out.error.message);
        if (!canRetry) {
            return out;
        }
        out.error.kind = MB_LLM_ERR_NONE;
        out.error.message[0] = '\0';
        if (!post_chat(a, cfg, systemText, userText, false, 0, &content, &out.error)) {
            return out;
        }
    }
    long elapsed = (long)mb_js_round(mb_now_ms() - started);

    /* --- validate --------------------------------------------------------- */
    MBJson* raw = mb_extract_json_object(a, content, &out.error);
    if (!raw) return out;

    out.result.plan = mb_normalize_plan(a, raw, &out.error);
    if (out.error.kind != MB_LLM_ERR_NONE) return out;

    snprintf(out.result.model, sizeof(out.result.model), "%s", cfg->model);
    {
        char* host = mb_url_host(a, cfg->baseUrl);
        snprintf(out.result.endpoint, sizeof(out.result.endpoint), "%s", host);
    }
    out.result.latencyMs = elapsed;
    out.result.raw = content;
    out.ok = true;
    return out;
}

MBLlmPlanOutcome mb_llm_ping(Arena* a, const MBLlmConfig* cfg)
{
    MBLlmPlanOutcome out;
    memset(&out, 0, sizeof(out));

    if (!cfg->baseUrl[0]) {
        mb_llm_error_set(&out.error, MB_LLM_ERR_UNKNOWN, "请先填写 base URL");
        return out;
    }

    char auth[300];
    auth[0] = '\0';
    if (cfg->apiKey[0]) snprintf(auth, sizeof(auth), "Bearer %s", cfg->apiKey);

    char* modelsUrl = mb_url_join(a, cfg->baseUrl, "/models");
    MBHttpRequest req;
    memset(&req, 0, sizeof(req));
    req.authorization = auth[0] ? auth : NULL;

    MBHttpResponse res;
    memset(&res, 0, sizeof(res));
    res.error = &out.error;

    bool haveModels = mb_http_do(a, "GET", modelsUrl, &req, &res);
    if (haveModels && res.status == 200) {
        MBJson* parsed = mb_json_parse(a, res.body ? res.body : "", res.bodyLen);
        const MBJson* data = parsed ? mb_json_get(parsed, "data") : NULL;
        int count = (data && data->type == MB_JSON_ARR) ? (int)data->u.arr.count : 0;
        bool hasModel = false;
        for (int i = 0; i < count; i++) {
            const MBJson* id = mb_json_get(mb_json_at(data, (size_t)i), "id");
            if (id && id->type == MB_JSON_STR && strcmp(id->u.str.ptr, cfg->model) == 0) {
                hasModel = true;
            }
        }
        out.error.kind = MB_LLM_ERR_NONE;
        out.ok = true;
        if (count > 0) {
            out.result.raw = hasModel
                ? mb_arena_printf(a, "连接成功，模型 %s 可用（共 %d 个模型）", cfg->model, count)
                : mb_arena_printf(a,
                      "连接成功，服务共有 %d 个模型，但列表里没有 %s，请核对模型名",
                      count, cfg->model);
        } else {
            out.result.raw = "连接成功";
        }
        snprintf(out.result.model, sizeof(out.result.model), "%s", cfg->model);
        return out;
    }

    /* 404/405 simply means the gateway has no /models — fall through. */
    if (haveModels && res.status != 404 && res.status != 405) {
        char msg[1024];
        MBLlmErrorKind kind = describe_http_error(res.status, res.body, msg, sizeof(msg));
        mb_llm_error_set(&out.error, kind, "%s", msg);
        return out;
    }

    /* Last resort: a real (tiny) completion. */
    out.error.kind = MB_LLM_ERR_NONE;
    out.error.message[0] = '\0';

    char* content = NULL;
    if (!post_chat(a, cfg, "你是一个助手。", "回复\"ok\"两个字符即可。",
                   false, 8, &content, &out.error)) {
        return out;
    }

    char* trimmed = mb_trim_dup(a, content);
    if (mb_u16_len(trimmed) > 20) {
        size_t n = 0;
        U16* u = mb_u16_from_utf8(a, trimmed, &n);
        trimmed = mb_utf8_from_u16(a, u, 20);
    }
    out.ok = true;
    out.result.raw = mb_arena_printf(a, "连接成功，模型有响应（%s）", trimmed);
    snprintf(out.result.model, sizeof(out.result.model), "%s", cfg->model);
    return out;
}

char* mb_redact(Arena* a, const char* text, const char* apiKey)
{
    const char* key = apiKey;
    if (!key) return mb_arena_strdup(a, text);
    while (*key == ' ' || *key == '\t' || *key == '\n' || *key == '\r') key++;
    size_t klen = strlen(key);
    while (klen > 0 && (key[klen - 1] == ' ' || key[klen - 1] == '\t' ||
                        key[klen - 1] == '\n' || key[klen - 1] == '\r')) klen--;
    if (klen < 8) return mb_arena_strdup(a, text);

    SBuf b;
    mb_sb_init(&b, a);
    const char* p = text;
    while (*p) {
        if (strncmp(p, key, klen) == 0) {
            mb_sb_puts(&b, "***");
            p += klen;
        } else {
            mb_sb_putc(&b, *p++);
        }
    }
    return mb_sb_done(&b);
}
