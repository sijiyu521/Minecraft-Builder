/*
 * main.c — the interactive terminal application.
 *
 * Layout, top to bottom:
 *
 *   header     title, engine badge, what the planner decided
 *   tabs       [1] 3D 预览  [2] 平面图  [3] 报告  [4] 导出  [5] 设置
 *   body       depends on the tab
 *   status     the last thing that happened, or an error in red
 *   hints      the keys that are live right now
 *
 * The app is modal: by default letters are commands, and pressing `i` hands the
 * keyboard to the prompt box. That is the only way to type ASCII letters into a
 * prompt without a forest of modifier keys.
 */
#include "mb.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

/* ========================================================================== */
/* constants                                                                  */
/* ========================================================================== */

#ifdef _WIN32
/*
 * The C runtime hands `main()` an argv in the ANSI code page. A Chinese prompt
 * then reaches the JSON body as GBK bytes, i.e. invalid UTF-8, and the model
 * endpoint rejects the request. Rebuild argv from the UTF-16 command line so
 * `mb --shot ... "中文提示词"` works; the scratch arena lives for the process.
 */
static void argv_as_utf8(Arena* a, int* argc, char*** argv)
{
    int wargc = 0;
    WCHAR** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv || wargc <= 0) return;

    char** out = (char**)mb_arena_alloc(a, (size_t)(wargc + 1) * sizeof(char*));
    if (!out) {
        LocalFree(wargv);
        return;
    }
    for (int i = 0; i < wargc; i++) {
        out[i] = mb_utf8_from_u16(a, (const U16*)wargv[i],
                                  mb_u16_len_buf((const U16*)wargv[i]));
    }
    out[wargc] = NULL;
    LocalFree(wargv);
    *argc = wargc;
    *argv = out;
}
#endif

enum { TAB_3D = 0, TAB_PLAN, TAB_REPORT, TAB_EXPORT, TAB_SETTINGS, TAB_COUNT };

static const char* TAB_NAMES[TAB_COUNT] = {
    "3D 预览", "平面图", "报告", "导出", "设置"
};

static const char* TAB_ICONS[TAB_COUNT] = { "◆", "▦", "≡", "⇩", "⚙" };

static const char* DEFAULT_PROMPT =
    "帮我建一座现代风格的两层别墅，白墙大落地窗，尺寸 15x11";

enum { SET_PRESET = 0, SET_URL, SET_KEY, SET_MODEL, SET_TEMP, SET_JSON, SET_COUNT };

static const char* SET_NAMES[SET_COUNT] = {
    "服务预设", "Base URL", "API Key", "模型名", "温度", "JSON 模式"
};

enum { EX_MCFUNCTION = 0, EX_JSON, EX_CSV, EX_MARKDOWN, EX_COUNT };

static const char* EX_NAMES[EX_COUNT]   = { ".mcfunction", ".json", ".csv", ".md" };
static const char* EX_DESCS[EX_COUNT]   = {
    "数据包函数，游戏内 /function 直接建造",
    "中性方案文件，可 diff、可复读",
    "材料清单，可直接喂给表格软件",
    "可读报告，适合贴进 README"
};

/* ========================================================================== */
/* app state                                                                  */
/* ========================================================================== */

typedef struct {
    char   prompt[512];
    char   status[512];
    char   error[1024];

    MBLlmConfig cfg;
    int         preset;

    MBView view;
    int    tab;
    int    layer;
    int    variant;

    bool   editing;

    int settingsRow;
    int exportRow;

    uint32_t seed;
    MBBuildResult* result;     /* lives in the model arena */

    Arena       planArena;     /* cached plan/trace strings survive variants */
    MBLlmPlan   plan;
    MBLlmTrace  trace;
    bool        hasPlan;
} App;

/* ========================================================================== */
/* text helpers                                                               */
/* ========================================================================== */

/* Display columns, not bytes. CJK is two cells wide, and ANSI escape sequences
   occupy no cells at all — mixed in with the text they must not be counted.
   The measurement lives in util.c so the renderer and the window agree with
   this frame on what "fits" means. */
static int display_width(const char* s)
{
    return mb_display_width(s);
}

/* ========================================================================== */
/* theme                                                                      */
/* ========================================================================== */
/*
 * One palette, shared by every surface: the terminal frame, the isometric
 * preview (render.c) and the window chrome (preview.c) all agree on these
 * values, so the product reads as a single piece of software.
 */

#define P_CANVAS   22, 24, 29
#define P_CARD     30, 33, 40
#define P_PANEL    38, 42, 51
#define P_ROW      44, 49, 60
#define P_LINE     51, 56, 69
#define P_INK      231, 234, 240
#define P_SOFT     195, 201, 214
#define P_DIM      139, 147, 167
#define P_ACCENT   76, 195, 138
#define P_ACCENT2  139, 123, 240
#define P_GOLD     224, 177, 94
#define P_ERR      255, 122, 122
#define P_ONACC    16, 24, 19

static void col_fg(SBuf* b, int r, int g, int bl)
{
    mb_sb_printf(b, "\x1b[38;2;%d;%d;%dm", r, g, bl);
}
static void col_bg(SBuf* b, int r, int g, int bl)
{
    mb_sb_printf(b, "\x1b[48;2;%d;%d;%dm", r, g, bl);
}
static void col_reset(SBuf* b) { mb_sb_puts(b, "\x1b[0m"); }

static const char* join_list(Arena* a, const char** items, int count)
{
    if (!items || count <= 0) return "无";
    SBuf b;
    mb_sb_init(&b, a);
    for (int i = 0; i < count; i++) {
        if (i) mb_sb_puts(&b, "、");
        mb_sb_puts(&b, items[i]);
    }
    return mb_sb_done(&b);
}

static void set_error(App* app, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(app->error, sizeof(app->error), fmt, ap);
    va_end(ap);
    app->status[0] = '\0';
}

/* ========================================================================== */
/* generation                                                                 */
/* ========================================================================== */

static uint32_t derive_seed(const App* app)
{
    return (mb_hash_string(app->prompt) + (uint32_t)app->variant * 7919u);
}

static bool ai_configured(const App* app)
{
    if (!app->cfg.baseUrl[0] || !app->cfg.model[0]) return false;
    bool keyOptional = app->preset >= 0 && app->preset < MB_LLM_PRESET_COUNT
                       && MB_LLM_PRESETS[app->preset].keyOptional;
    return app->cfg.apiKey[0] != '\0' || keyOptional;
}

static void do_ai(App* app, Arena* model)
{
    if (!ai_configured(app)) {
        set_error(app, "请先在设置页填写服务地址、模型名与 API Key（Ollama 可留空 Key）");
        app->result = NULL;
        app->hasPlan = false;
        return;
    }

    mb_arena_reset(&app->planArena);
    MBLlmPlanOutcome outcome = mb_llm_plan(&app->planArena, app->prompt, &app->cfg,
                                           NULL, NULL);
    if (!outcome.ok) {
        set_error(app, "[%s] %s", mb_llm_error_kind_name(outcome.error.kind),
                  outcome.error.message);
        app->result = NULL;
        app->hasPlan = false;
        return;
    }

    app->plan = outcome.result.plan;
    memset(&app->trace, 0, sizeof(app->trace));
    app->trace.model = mb_arena_strdup(&app->planArena, outcome.result.model);
    app->trace.endpoint = mb_arena_strdup(&app->planArena, outcome.result.endpoint);
    app->trace.latencyMs = outcome.result.latencyMs;
    app->trace.reasoning = app->plan.notes;
    app->trace.reasoningCount = app->plan.noteCount;
    app->hasPlan = true;

    app->result = mb_generate(model, app->prompt, app->seed, &app->plan, &app->trace);

    if (app->result) {
        snprintf(app->status, sizeof(app->status),
                 "模型规划完成：%s · %ld ms", outcome.result.model, outcome.result.latencyMs);
        app->error[0] = '\0';
    } else {
        set_error(app, "建筑生成失败");
    }
}

static void regenerate(App* app, Arena* model)
{
    mb_arena_reset(model);
    mb_arena_reset(&app->planArena);
    app->result = NULL;
    app->hasPlan = false;
    app->seed = derive_seed(app);
    app->layer = 0;
    do_ai(app, model);
}

/* "换一个变体" reuses the cached model intent and only re-rolls the seed, so it
 * costs no extra tokens. */
static void regenerate_variant(App* app, Arena* model)
{
    if (!app->hasPlan) {
        set_error(app, "请先用大模型生成一次建筑");
        return;
    }
    app->variant++;
    mb_arena_reset(model);
    app->result = NULL;
    app->seed = derive_seed(app);
    app->layer = 0;
    app->result = mb_generate(model, app->prompt, app->seed, &app->plan, &app->trace);
    if (!app->result) {
        set_error(app, "建筑生成失败");
        return;
    }
    snprintf(app->status, sizeof(app->status), "已换一个变体（种子 %u）", (unsigned)app->seed);
    app->error[0] = '\0';
}

/* ========================================================================== */
/* drawing                                                                    */
/* ========================================================================== */

static void draw_tabs(SBuf* b, const App* app, int cols)
{
    (void)cols;
    mb_sb_puts(b, "\x1b[0m");
    for (int i = 0; i < TAB_COUNT; i++) {
        char label[64];
        snprintf(label, sizeof(label), " %s %s ", TAB_ICONS[i], TAB_NAMES[i]);
        if (i == app->tab) {
            col_bg(b, P_ACCENT);
            col_fg(b, P_ONACC);
            mb_sb_puts(b, "\x1b[1m");
            mb_sb_puts(b, label);
            col_reset(b);
        } else {
            col_fg(b, P_DIM);
            mb_sb_puts(b, label);
            col_reset(b);
        }
        mb_sb_putc(b, ' ');
    }
    col_reset(b);
    mb_sb_puts(b, "\x1b[K\n");
}

/* A section heading inside a card: a small accent marker plus a bold title. */
static void card_title(SBuf* b, const char* text, int cols)
{
    col_fg(b, P_ACCENT);
    mb_sb_puts(b, "  ▎ ");
    col_fg(b, P_INK);
    mb_sb_puts(b, "\x1b[1m");
    int tw = mb_sb_puts_clip(b, text, strlen(text), cols - 4);
    col_reset(b);
    int w = 4 + tw;
    for (int i = w; i < cols; i++) mb_sb_putc(b, ' ');
    mb_sb_puts(b, "\x1b[K\n");
}

/* A key/value line: muted key on the left, ink value after it. Both fields are
   clipped to the card, so a long prompt or Base URL truncates instead of
   wrapping the whole frame. */
#define KV_KEYW 13
static void card_kv(SBuf* b, const char* key, const char* value, int cols)
{
    col_fg(b, P_DIM);
    mb_sb_puts(b, "   ");
    int kw = mb_sb_puts_clip(b, key, strlen(key), KV_KEYW);
    for (int i = kw; i < KV_KEYW; i++) mb_sb_putc(b, ' ');
    col_fg(b, P_SOFT);
    int avail = cols - (3 + KV_KEYW);
    if (avail < 0) avail = 0;
    int vw = mb_sb_puts_clip(b, value, strlen(value), avail);
    int used = 3 + KV_KEYW + vw;
    col_reset(b);
    for (int i = used; i < cols; i++) mb_sb_putc(b, ' ');
    mb_sb_puts(b, "\x1b[K\n");
}

/* A selectable row. The active row gets a filled accent bar and a highlighted
   background, so the cursor is unmistakable at a glance. The label and the
   trailing value are clipped to the card width. */
#define ROW_FIELDW 40
static void card_row(SBuf* b, bool active, const char* left, const char* right, int cols)
{
    int fieldW = ROW_FIELDW < cols ? ROW_FIELDW : cols;
    int leftMax = fieldW - 4;
    if (leftMax < 1) leftMax = 1;

    if (active) {
        col_bg(b, P_ROW);
        col_fg(b, P_ACCENT);
        mb_sb_puts(b, "  ▎ ");
        col_fg(b, P_INK);
        mb_sb_puts(b, "\x1b[1m");
        int w = mb_sb_puts_clip(b, left, strlen(left), leftMax);
        mb_sb_puts(b, "\x1b[22m");
        col_fg(b, P_SOFT);
        for (int i = 4 + w; i < fieldW; i++) mb_sb_putc(b, ' ');
    } else {
        col_fg(b, P_DIM);
        mb_sb_puts(b, "    ");
        int w = mb_sb_puts_clip(b, left, strlen(left), leftMax);
        for (int i = 4 + w; i < fieldW; i++) mb_sb_putc(b, ' ');
    }

    int used = fieldW;
    if (right) {
        int avail = cols - fieldW;
        if (avail > 0) used = fieldW + mb_sb_puts_clip(b, right, strlen(right), avail);
    }
    for (int i = used; i < cols; i++) mb_sb_putc(b, ' ');
    col_reset(b);
    mb_sb_puts(b, "\x1b[K\n");
}

static void card_blank(SBuf* b, int cols)
{
    (void)cols;
    mb_sb_puts(b, "\x1b[K\n");
}

static void draw_body_3d(SBuf* b, App* app, Arena* frame, int cols, int bodyRows)
{
    const MBBuildResult* r = app->result;
    if (!r) {
        card_blank(b, cols);
        card_kv(b, "预览", "还没有建筑 — 配置模型后按 Enter 生成", cols);
        for (int i = 2; i < bodyRows; i++) mb_sb_puts(b, "\x1b[K\n");
        return;
    }

    /* The legend rows are laid out to 35 columns (see LEGEND_* in render.c), so
       the panel must reserve at least 36 or the last column gets clipped. */
    bool withLegend = cols >= 104;
    int legendW = withLegend ? (cols >= 130 ? 44 : 36) : 0;
    int isoCols = cols - (withLegend ? legendW + 3 : 0);
    if (isoCols < 20) { withLegend = false; legendW = 0; isoCols = cols; }

    char* iso = mb_render_iso(frame, r, &app->view, isoCols, bodyRows);
    char* legend = withLegend ? mb_render_legend(frame, r) : NULL;

    const char* isoPtr = iso;
    const char* legPtr = legend;
    for (int i = 0; i < bodyRows; i++) {
        const char* isoEnd = strchr(isoPtr, '\n');
        const char* legEnd = legPtr ? strchr(legPtr, '\n') : NULL;

        if (isoEnd) mb_sb_write(b, isoPtr, (size_t)(isoEnd - isoPtr));
        else        for (int k = 0; k < isoCols; k++) mb_sb_putc(b, ' ');

        if (withLegend) {
            col_fg(b, P_LINE);
            mb_sb_puts(b, " │ ");
            col_reset(b);
            if (legEnd) {
                /* Hard clamp: the legend generator already fits its budget, but
                   the panel is the authority — clip and pad here so a stray wide
                   glyph can never push the row past `legendW` and wrap. */
                int lw = mb_sb_puts_clip(b, legPtr, (size_t)(legEnd - legPtr), legendW);
                for (int k = lw; k < legendW; k++) mb_sb_putc(b, ' ');
            } else if (i == 0) {
                col_fg(b, P_DIM);
                mb_sb_puts(b, "（无材料）");
                col_reset(b);
            }
        }
        col_reset(b);
        mb_sb_puts(b, "\x1b[K\n");

        if (isoEnd) isoPtr = isoEnd + 1;
        if (legEnd) legPtr = legEnd + 1;
        if (!*isoPtr) isoPtr = "";
    }
}

static void draw_body_plan(SBuf* b, App* app, Arena* frame, int cols, int bodyRows)
{
    const MBBuildResult* r = app->result;
    if (!r) {
        card_blank(b, cols);
        card_kv(b, "平面图", "没有可绘制的建筑", cols);
        for (int i = 2; i < bodyRows; i++) mb_sb_puts(b, "\x1b[K\n");
        return;
    }

    char head[128];
    int totalLayers = r->size.y > 0 ? r->size.y - 1 : 0;
    snprintf(head, sizeof(head), "平面图 · 第 %d 层 / 共 %d 层   [ ] 切换楼层",
             app->layer, totalLayers);
    card_title(b, head, cols);

    char* plan = mb_render_slice(frame, r, app->layer, cols, bodyRows - 1);
    const char* p = plan;
    for (int i = 1; i < bodyRows; i++) {
        const char* end = strchr(p, '\n');
        if (end) {
            mb_sb_write(b, p, (size_t)(end - p));
            mb_sb_puts(b, "\x1b[0m\x1b[K\n");
            p = end + 1;
        } else {
            mb_sb_puts(b, "\x1b[K\n");
        }
    }
}

static void draw_body_report(SBuf* b, App* app, Arena* frame, int cols, int bodyRows)
{
    const MBBuildResult* r = app->result;
    if (!r) {
        card_blank(b, cols);
        card_kv(b, "报告", "还没有报告 — 生成一次后显示", cols);
        for (int i = 2; i < bodyRows; i++) mb_sb_puts(b, "\x1b[K\n");
        return;
    }

    const MBPlannerReport* p = &r->report;
    char line[1024];
    int used = 0;

#define RPT(key, fmt, ...)                                                   \
    do {                                                                     \
        if (used < bodyRows) {                                               \
            snprintf(line, sizeof(line), fmt, __VA_ARGS__);                  \
            card_kv(b, key, line, cols);                                     \
            used++;                                                          \
        }                                                                    \
    } while (0)

#define RPTV(key, val)                                                       \
    do {                                                                     \
        if (used < bodyRows) { card_kv(b, key, (val), cols); used++; }       \
    } while (0)

#define SECTION(title)                                                       \
    do {                                                                     \
        if (used < bodyRows) { card_blank(b, cols); used++; }                \
        if (used < bodyRows) { card_title(b, title, cols); used++; }         \
    } while (0)

    SECTION("方案");
    RPT("名称", "%s", r->name);
    RPT("摘要", "%s", r->summary);

    SECTION("原始输入");
    RPT("提示词", "%s", app->prompt);
    RPT("建筑类型", "%s", p->structureLabel);
    RPT("体量", "%s", p->scale);
    {
        char sd[32];
        snprintf(sd, sizeof(sd), "%u", (unsigned)p->seed);
        RPTV("种子", sd);
    }

    SECTION("识别结果");
    RPTV("关键词", join_list(frame, p->keywords, p->keywordCount));
    RPTV("材质", join_list(frame, p->detectedMaterials, p->detectedMaterialCount));
    RPTV("修饰语", join_list(frame, p->modifiers, p->modifierCount));

    SECTION("几何统计");
    RPT("尺寸", "宽 %d · 高 %d · 深 %d", r->size.x, r->size.y, r->size.z);
    RPT("方块总数", "%d 块", r->total);
    RPT("材质种类", "%d 种", r->byMaterialCount);

    SECTION("规划引擎");
    RPT("模型", "%s", p->hasLlmUser ? p->llm.model : "未知");
    RPT("接口", "%s", p->hasLlmUser ? p->llm.endpoint : "未知");
    for (int i = 0; i < p->noteCount && used < bodyRows; i++) {
        RPT("备注", "%s", p->notes[i]);
    }
#undef RPT
#undef RPTV
#undef SECTION

    for (; used < bodyRows; used++) mb_sb_puts(b, "\x1b[K\n");
}

static char* export_text(Arena* a, App* app, int which)
{
    const MBBuildResult* r = app->result;
    if (!r) return NULL;
    switch (which) {
    case EX_MCFUNCTION: return mb_export_mcfunction(a, r, "builder");
    case EX_JSON:       return mb_export_json(a, r);
    case EX_CSV:        return mb_export_csv(a, r);
    case EX_MARKDOWN:   return mb_export_markdown(a, r);
    default:            return NULL;
    }
}

static void export_base_name(const App* app, char* out, size_t size)
{
    const char* id = app->result ? app->result->report.structure : "build";
    snprintf(out, size, "%s-%u", id, (unsigned)app->seed);
}

static void draw_body_export(SBuf* b, App* app, Arena* frame, int cols, int bodyRows)
{
    (void)frame;
    if (!app->result) {
        card_blank(b, cols);
        card_kv(b, "导出", "没有可导出的建筑", cols);
        for (int i = 2; i < bodyRows; i++) mb_sb_puts(b, "\x1b[K\n");
        return;
    }

    char base[128];
    export_base_name(app, base, sizeof(base));

    int used = 0;
    card_title(b, "导出文件", cols); used++;
    card_kv(b, "提示", "选中后按 Enter 写出到当前目录：", cols); used++;
    card_blank(b, cols); used++;

    for (int i = 0; i < EX_COUNT && used < bodyRows; i++) {
        char left[256];
        snprintf(left, sizeof(left), "%s", EX_NAMES[i]);
        card_row(b, i == app->exportRow, left, NULL, cols);
        used++;
        if (used < bodyRows) {
            char desc[320];
            snprintf(desc, sizeof(desc), "%s%s", base, EX_NAMES[i]);
            col_fg(b, P_DIM);
            mb_sb_puts(b, "      ");
            mb_sb_puts(b, EX_DESCS[i]);
            mb_sb_puts(b, "  ");
            col_fg(b, P_ACCENT2);
            mb_sb_puts(b, desc);
            col_reset(b);
            mb_sb_puts(b, "\x1b[K\n");
            used++;
        }
    }

    if (app->status[0] && used < bodyRows) {
        card_blank(b, cols); used++;
        col_fg(b, P_ACCENT);
        mb_sb_puts(b, "  ✓ 已写出 ");
        col_fg(b, P_SOFT);
        mb_sb_puts(b, app->status);
        col_reset(b);
        mb_sb_puts(b, "\x1b[K\n");
        used++;
    }

    for (; used < bodyRows; used++) mb_sb_puts(b, "\x1b[K\n");
}

static void draw_body_settings(SBuf* b, App* app, int cols, int bodyRows)
{
    int used = 0;
    card_title(b, "大模型接入", cols); used++;
    card_kv(b, "提示", "填好地址与模型名后，按 1 回到 3D 预览，再按 Enter 生成。", cols); used++;
    card_blank(b, cols); used++;

    for (int i = 0; i < SET_COUNT && used < bodyRows; i++) {
        char value[384];
        switch (i) {
        case SET_PRESET:
            snprintf(value, sizeof(value), "%s  <%s>", MB_LLM_PRESETS[app->preset].label,
                     MB_LLM_PRESETS[app->preset].id);
            break;
        case SET_URL:
            snprintf(value, sizeof(value), "%s", app->cfg.baseUrl);
            break;
        case SET_KEY:
            if (app->cfg.apiKey[0]) snprintf(value, sizeof(value), "••••••••  已设置（回车可改）");
            else snprintf(value, sizeof(value), "未设置%s",
                          MB_LLM_PRESETS[app->preset].keyOptional ? "  — 该服务可留空" : "");
            break;
        case SET_MODEL:
            snprintf(value, sizeof(value), "%s", app->cfg.model);
            break;
        case SET_TEMP:
            snprintf(value, sizeof(value), "%.2f   ← → 微调", app->cfg.temperature);
            break;
        case SET_JSON:
            snprintf(value, sizeof(value), "%s   ← → 切换",
                     app->cfg.jsonMode ? "开启" : "关闭");
            break;
        default:
            value[0] = '\0';
        }

        card_row(b, i == app->settingsRow, SET_NAMES[i], value, cols);
        used++;
    }

    if (used < bodyRows) {
        card_blank(b, cols); used++;
        col_fg(b, P_DIM);
        mb_sb_puts(b, "  ");
        mb_sb_puts(b, MB_LLM_PRESETS[app->preset].hint);
        col_reset(b);
        mb_sb_puts(b, "\x1b[K\n");
        used++;
    }

    for (; used < bodyRows; used++) mb_sb_puts(b, "\x1b[K\n");
}

static void draw(App* app, Arena* frame, int cols, int rows)
{
    mb_arena_reset(frame);

    SBuf b;
    mb_sb_init(&b, frame);
    mb_sb_puts(&b, "\x1b[?25l\x1b[H");

    int lines = rows - 1;
    int bodyRows = lines - 4;
    if (bodyRows < 6) bodyRows = 6;

    /* --- header ---------------------------------------------------------- */
    {
        const MBBuildResult* r = app->result;
        char title[128];
        snprintf(title, sizeof(title), "  \xe2\x9b\x8f  Minecraft Builder");

        char engine[192];
        snprintf(engine, sizeof(engine), " 大模型 · %s ",
                 app->cfg.model[0] ? app->cfg.model : "未配置");

        col_bg(&b, P_CARD);
        col_fg(&b, P_ACCENT);
        mb_sb_puts(&b, "\x1b[1m");
        mb_sb_puts(&b, title);
        col_fg(&b, P_DIM);
        mb_sb_puts(&b, "\x1b[22m  ·  C 版");

        int usedW = display_width(title) + display_width("  ·  C 版");
        int engineW = display_width(engine);
        for (int i = usedW; i < cols - engineW - 2; i++) mb_sb_putc(&b, ' ');

        col_bg(&b, P_ACCENT2);
        col_fg(&b, P_INK);
        mb_sb_puts(&b, "\x1b[1m");
        mb_sb_puts(&b, engine);
        col_reset(&b);
        mb_sb_puts(&b, "\x1b[K\n");

        if (r) {
            char mid[320];
            snprintf(mid, sizeof(mid), "  %s · %d×%d 占地 · 高 %d · %d 方块",
                     r->name, r->size.x, r->size.z, r->size.y, r->total);
            col_bg(&b, P_CARD);
            col_fg(&b, P_GOLD);
            mb_sb_puts(&b, mid);
            int mw = display_width(mid);
            col_fg(&b, P_DIM);
            for (int i = mw; i < cols; i++) mb_sb_putc(&b, ' ');
            col_reset(&b);
            mb_sb_puts(&b, "\x1b[K\n");
        } else {
            col_bg(&b, P_CARD);
            col_fg(&b, P_DIM);
            mb_sb_puts(&b, "  ");
            for (int i = 3; i < cols; i++) mb_sb_putc(&b, ' ');
            col_reset(&b);
            mb_sb_puts(&b, "\x1b[K\n");
        }
        col_reset(&b);
    }

    draw_tabs(&b, app, cols);

    /* --- body ------------------------------------------------------------ */
    switch (app->tab) {
    case TAB_3D:      draw_body_3d(&b, app, frame, cols, bodyRows); break;
    case TAB_PLAN:    draw_body_plan(&b, app, frame, cols, bodyRows); break;
    case TAB_REPORT:  draw_body_report(&b, app, frame, cols, bodyRows); break;
    case TAB_EXPORT:  draw_body_export(&b, app, frame, cols, bodyRows); break;
    default:          draw_body_settings(&b, app, cols, bodyRows); break;
    }

    /* --- status ---------------------------------------------------------- */
    col_bg(&b, P_CARD);
    if (app->error[0]) {
        col_fg(&b, P_ERR);
        mb_sb_puts(&b, "  \xe2\x9c\x97 ");
        mb_sb_puts(&b, app->error);
        col_reset(&b);
        mb_sb_puts(&b, "\x1b[K\n");
    } else if (app->status[0]) {
        col_fg(&b, P_ACCENT);
        mb_sb_puts(&b, "  \xe2\x9c\x93 ");
        col_fg(&b, P_SOFT);
        mb_sb_puts(&b, app->status);
        col_reset(&b);
        mb_sb_puts(&b, "\x1b[K\n");
    } else if (app->editing) {
        col_fg(&b, P_ACCENT);
        mb_sb_puts(&b, "  \xe2\x9c\x8e 提示词  ");
        col_fg(&b, P_INK);
        mb_sb_puts(&b, app->prompt);
        col_fg(&b, P_ACCENT);
        mb_sb_puts(&b, "\xe2\x96\x88");
        col_reset(&b);
        mb_sb_puts(&b, "\x1b[K\n");
    } else {
        col_fg(&b, P_DIM);
        mb_sb_puts(&b, "  就绪");
        col_reset(&b);
        mb_sb_puts(&b, "\x1b[K\n");
    }

    /* --- hints ----------------------------------------------------------- */
    col_bg(&b, P_CANVAS);
    {
        const char* hint;
        if (app->editing) {
            hint = " 输入中：直接打字 · Backspace 删除 · Enter 生成 · Esc 取消";
        } else if (app->tab == TAB_SETTINGS) {
            hint = " ↑↓ 选择字段 · ←→ 调整 · Enter 编辑/切换 · Tab 切换视图 · q 退出";
        } else if (app->tab == TAB_EXPORT) {
            hint = " ↑↓ 选择 · Enter 写出文件 · Tab 切换视图 · i 编辑提示词 · q 退出";
        } else if (app->tab == TAB_PLAN) {
            hint = " [ ] 切换楼层 · ←→↑↓ 视角 · - + 缩放 · Tab 切换视图 · i 编辑提示词 · Enter 生成 · q 退出";
        } else {
            hint = " i 编辑提示词 · Enter 生成（调用模型） · g 换变体 · t 测试连接 · Tab 切换 · ←→↑↓ 视角 · p 3D 窗口 · q 退出";
        }
        col_fg(&b, P_DIM);
        mb_sb_puts(&b, hint);
        col_reset(&b);
        mb_sb_puts(&b, "\x1b[K");
    }

    mb_sb_puts(&b, "\x1b[0m");
    mb_term_write(mb_sb_done(&b));
}

/* ========================================================================== */
/* interaction                                                                */
/* ========================================================================== */

static bool write_text_file(const char* path, const char* text)
{
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    size_t n = strlen(text);
    bool ok = fwrite(text, 1, n, f) == n;
    fclose(f);
    return ok;
}

static void do_export(App* app, Arena* frame)
{
    if (!app->result) {
        set_error(app, "没有可导出的建筑。");
        return;
    }
    char* text = export_text(frame, app, app->exportRow);
    if (!text) { set_error(app, "导出失败：内容为空。"); return; }

    char base[128];
    export_base_name(app, base, sizeof(base));

    char path[256];
    snprintf(path, sizeof(path), "%s%s", base, EX_NAMES[app->exportRow]);

    if (!write_text_file(path, text)) {
        set_error(app, "无法写入 %s（检查当前目录是否可写）。", path);
        return;
    }

    /* The arena that `frame` points at is about to be reset, so the path has to
       be copied somewhere that outlives it. */
    char keep[256];
    snprintf(keep, sizeof(keep), "%s", path);
    mb_arena_reset(frame);

    snprintf(app->status, sizeof(app->status), "%s", keep);
    app->error[0] = '\0';
}

static void cycle_preset(App* app, int delta)
{
    app->preset = (app->preset + delta + MB_LLM_PRESET_COUNT) % MB_LLM_PRESET_COUNT;
    snprintf(app->status, sizeof(app->status), "已选择预设 %s",
             MB_LLM_PRESETS[app->preset].label);
    app->error[0] = '\0';
}

static void apply_preset(App* app)
{
    const MBLlmPreset* p = &MB_LLM_PRESETS[app->preset];
    snprintf(app->cfg.baseUrl, sizeof(app->cfg.baseUrl), "%s", p->baseUrl);
    snprintf(app->cfg.model, sizeof(app->cfg.model), "%s", p->model);
    snprintf(app->status, sizeof(app->status), "已套用 %s 的默认地址与模型", p->label);
    app->error[0] = '\0';
}

static void edit_settings_row(App* app, Arena* frame)
{
    switch (app->settingsRow) {
    case SET_PRESET:
        apply_preset(app);
        break;
    case SET_URL: {
        char label[320];
        snprintf(label, sizeof(label), "Base URL [%s]: ", app->cfg.baseUrl);
        char* v = mb_term_input_line(frame, label);
        if (v[0]) snprintf(app->cfg.baseUrl, sizeof(app->cfg.baseUrl), "%s", v);
        break;
    }
    case SET_KEY: {
        char* v = mb_term_input_line(frame, "API Key（留空保持不变）: ");
        if (v[0]) snprintf(app->cfg.apiKey, sizeof(app->cfg.apiKey), "%s", v);
        break;
    }
    case SET_MODEL: {
        char label[256];
        snprintf(label, sizeof(label), "模型名 [%s]: ", app->cfg.model);
        char* v = mb_term_input_line(frame, label);
        if (v[0]) snprintf(app->cfg.model, sizeof(app->cfg.model), "%s", v);
        break;
    }
    case SET_TEMP: {
        char label[128];
        snprintf(label, sizeof(label), "温度 0.0-2.0 [%.2f]: ", app->cfg.temperature);
        char* v = mb_term_input_line(frame, label);
        if (v[0]) {
            double t = atof(v);
            if (t < 0.0) t = 0.0;
            if (t > 2.0) t = 2.0;
            app->cfg.temperature = t;
        }
        break;
    }
    default:
        app->cfg.jsonMode = !app->cfg.jsonMode;
        break;
    }
    mb_arena_reset(frame);
    snprintf(app->status, sizeof(app->status), "设置已更新");
    app->error[0] = '\0';
}

static void test_connection(App* app, Arena* model)
{
    if (app->cfg.baseUrl[0] == '\0' || app->cfg.model[0] == '\0') {
        set_error(app, "请先在设置里填写 Base URL 与模型名。");
        return;
    }

    mb_term_write("\x1b[H\x1b[0m\x1b[K正在连接模型服务…\n");
    mb_term_write("\x1b[0m\x1b[K");

    MBLlmPlanOutcome outcome = mb_llm_ping(model, &app->cfg);
    if (!outcome.ok) {
        set_error(app, "[%s] %s", mb_llm_error_kind_name(outcome.error.kind),
                  outcome.error.message);
        return;
    }

    snprintf(app->status, sizeof(app->status), "%s", outcome.result.raw);
    app->error[0] = '\0';
}

static void backspace_utf8(char* s)
{
    size_t n = strlen(s);
    if (n == 0) return;
    n--;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    s[n] = '\0';
}

/*
 * Hand the current build to the windowed preview. It is modal: the terminal
 * keeps its raw mode and simply waits while the window pumps its own messages.
 */
static void open_preview(App* app)
{
    if (!app->result) {
        set_error(app, "没有可预览的建筑，请先生成一次。");
        return;
    }

    char title[256];
    snprintf(title, sizeof(title), "%s · %d×%d×%d",
             app->result->name, app->result->size.x,
             app->result->size.y, app->result->size.z);

    if (!mb_preview_open(app->result, title, false)) {
        set_error(app, "无法打开 3D 预览窗口（需要 Windows 桌面环境）。");
        return;
    }
    snprintf(app->status, sizeof(app->status), "已关闭 3D 预览窗口。");
}

static void handle_char(App* app, int key, Arena* model)
{
    switch (key) {
    case MB_KEY_ENTER:
        if (app->editing) {
            app->editing = false;
            regenerate(app, model);
        } else if (app->tab == TAB_EXPORT) {
            Arena scratch;
            mb_arena_init(&scratch);
            do_export(app, &scratch);
            mb_arena_destroy(&scratch);
        } else if (app->tab == TAB_SETTINGS) {
            Arena scratch;
            mb_arena_init(&scratch);
            edit_settings_row(app, &scratch);
            mb_arena_destroy(&scratch);
        } else {
            regenerate(app, model);
        }
        return;
    case MB_KEY_BACKSPACE:
        if (app->editing) backspace_utf8(app->prompt);
        return;
    case MB_KEY_ESC:
        app->editing = false;
        app->status[0] = '\0';
        app->error[0] = '\0';
        return;
    case MB_KEY_TAB:
        app->editing = false;
        app->tab = (app->tab + 1) % TAB_COUNT;
        app->error[0] = '\0';
        return;
    default:
        break;
    }

    if (app->editing) {
        if (key >= MB_KEY_CHAR) {
            unsigned char c = (unsigned char)(key - MB_KEY_CHAR);
            size_t n = strlen(app->prompt);
            if (c >= 32 && n + 1 < sizeof(app->prompt)) {
                app->prompt[n] = (char)c;
                app->prompt[n + 1] = '\0';
            }
        }
        return;
    }

    /* --- navigation mode ------------------------------------------------- */
    switch (app->tab) {
    case TAB_SETTINGS:
        if (key == MB_KEY_UP)   { app->settingsRow = (app->settingsRow + SET_COUNT - 1) % SET_COUNT; return; }
        if (key == MB_KEY_DOWN) { app->settingsRow = (app->settingsRow + 1) % SET_COUNT; return; }
        if (key == MB_KEY_LEFT) {
            if (app->settingsRow == SET_PRESET) cycle_preset(app, -1);
            else if (app->settingsRow == SET_TEMP) {
                app->cfg.temperature -= 0.1; if (app->cfg.temperature < 0.0) app->cfg.temperature = 0.0;
            } else if (app->settingsRow == SET_JSON) app->cfg.jsonMode = !app->cfg.jsonMode;
            return;
        }
        if (key == MB_KEY_RIGHT) {
            if (app->settingsRow == SET_PRESET) cycle_preset(app, 1);
            else if (app->settingsRow == SET_TEMP) {
                app->cfg.temperature += 0.1; if (app->cfg.temperature > 2.0) app->cfg.temperature = 2.0;
            } else if (app->settingsRow == SET_JSON) app->cfg.jsonMode = !app->cfg.jsonMode;
            return;
        }
        break;
    case TAB_EXPORT:
        if (key == MB_KEY_UP)   { app->exportRow = (app->exportRow + EX_COUNT - 1) % EX_COUNT; return; }
        if (key == MB_KEY_DOWN) { app->exportRow = (app->exportRow + 1) % EX_COUNT; return; }
        break;
    case TAB_PLAN:
        if (key == MB_KEY_LEFT || key == '[') {
            if (app->layer > 0) app->layer--;
            return;
        }
        if (key == MB_KEY_RIGHT || key == ']') {
            if (app->result && app->layer < app->result->size.y - 1) app->layer++;
            return;
        }
        break;
    default:
        break;
    }

    /* 3D view controls */
    if (key == MB_KEY_LEFT)  { app->view.yaw -= 0.1308996938995747; return; }
    if (key == MB_KEY_RIGHT) { app->view.yaw += 0.1308996938995747; return; }
    if (key == MB_KEY_UP)    { app->view.pitch += 0.06; if (app->view.pitch > 1.35) app->view.pitch = 1.35; return; }
    if (key == MB_KEY_DOWN)  { app->view.pitch -= 0.06; if (app->view.pitch < 0.12) app->view.pitch = 0.12; return; }

    if (key >= MB_KEY_CHAR) {
        char c = (char)(key - MB_KEY_CHAR);
        switch (c) {
        case 'i':
            app->editing = true;
            app->status[0] = '\0';
            app->error[0] = '\0';
            break;
        case 'g':
            regenerate_variant(app, model);
            break;
        case 't': {
            Arena scratch;
            mb_arena_init(&scratch);
            test_connection(app, &scratch);
            mb_arena_destroy(&scratch);
            break;
        }
        case 'p':
            open_preview(app);
            break;
        case '[':
            if (app->layer > 0) app->layer--;
            break;
        case ']':
            if (app->result && app->layer < app->result->size.y - 1) app->layer++;
            break;
        case '-':
            app->view.zoom = app->view.zoom > 0.0 ? app->view.zoom * 0.85 : 6.0;
            if (app->view.zoom < 0.4) app->view.zoom = 0.4;
            break;
        case '+':
        case '=':
            app->view.zoom = app->view.zoom > 0.0 ? app->view.zoom * 1.18 : 6.0;
            if (app->view.zoom > 60.0) app->view.zoom = 60.0;
            break;
        case '0':
            app->view.zoom = 0.0;   /* back to auto-fit */
            break;
        case '1': app->tab = TAB_3D;       break;
        case '2': app->tab = TAB_PLAN;     break;
        case '3': app->tab = TAB_REPORT;   break;
        case '4': app->tab = TAB_EXPORT;   break;
        case '5': app->tab = TAB_SETTINGS; break;
        case 'v': app->view.showGrid = !app->view.showGrid; break;
        case 'x': app->view.showAxes = !app->view.showAxes; break;
        default: break;
        }
    }
}

/* ========================================================================== */
/* entry point                                                                */
/* ========================================================================== */

static void cli_usage(void)
{
    printf(
        "Minecraft Builder — C 版\n"
        "\n"
        "  mb                    启动桌面图形界面\n"
        "  mb --tui              启动终端界面\n"
        "  mb --preview [提示词]  调用模型规划并打开 3D 预览窗口\n"
        "  mb --shot 文件.bmp    调用模型规划、离屏渲染一帧并写出 BMP\n"
        "  mb --size 1280x720    指定 --shot 的输出尺寸，默认 960x600\n"
        "  mb --yaw 45           覆盖相机水平角（度，仅对 --shot 有效）\n"
        "  mb --pitch -28        覆盖相机俯仰角（度，仅对 --shot 有效）\n"
        "  mb --dist 12          覆盖相机距离（方块，仅对 --shot 有效）\n"
        "  mb --help             显示这段说明\n"
        "\n"
        "--shot / --preview 也需要模型配置：先在桌面版的「模型设置」里填好\n"
        "服务地址、模型名和 API Key（或 Ollama 本地服务），配置会自动读取。\n"
        "\n"
        "提示词也可以直接写在命令行上，例如：\n"
        "  mb --preview 帮我建一座海岛小屋\n");
}

/*
 * Headless generation goes through the same model path as the GUI: load the
 * saved configuration, ask the model for a build intent, then hand that intent
 * to the deterministic builders. --shot/--preview are therefore network paths.
 */
static MBBuildResult* cli_generate(Arena* a, const char* prompt)
{
    MBLlmConfig cfg;
    mb_llm_default_config(&cfg);
    mb_config_load(&cfg);

    if (!cfg.baseUrl[0] || !cfg.model[0]) {
        fprintf(stderr,
                "没有可用的模型配置：请先启动 mb，在「模型设置」里填写服务地址、"
                "模型名和 API Key 并保存（Ollama 本地服务可留空 Key），再重试。\n");
        return NULL;
    }

    MBLlmPlanOutcome outcome = mb_llm_plan(a, prompt, &cfg, NULL, NULL);
    if (!outcome.ok) {
        fprintf(stderr, "模型规划失败：[%s] %s\n",
                mb_llm_error_kind_name(outcome.error.kind), outcome.error.message);
        return NULL;
    }

    MBLlmTrace trace;
    memset(&trace, 0, sizeof(trace));
    trace.model = mb_arena_strdup(a, outcome.result.model);
    trace.endpoint = mb_arena_strdup(a, outcome.result.endpoint);
    trace.latencyMs = outcome.result.latencyMs;
    trace.reasoning = outcome.result.plan.notes;
    trace.reasoningCount = outcome.result.plan.noteCount;

    MBBuildResult* r = mb_generate(a, prompt, mb_hash_string(prompt),
                                   &outcome.result.plan, &trace);
    if (!r) fprintf(stderr, "建筑生成失败。\n");
    return r;
}

/*
 * Headless frame. This is the path the regression tests use, and the one that
 * produced the screenshots in the README: it renders exactly the same scene the
 * window shows, without needing a window or a display.
 */
static bool cli_write_shot(Arena* arena, const MBBuildResult* r,
                           const char* path, int w, int h,
                           double yawDeg, double pitchDeg, double dist)
{
    MBFramebuffer fb;
    mb_fb_init(&fb, arena, w, h);

    MBCamera cam;
    mb_camera_default(&cam, r);

    if (yawDeg != 0.0 || pitchDeg != 0.0) {
        const double toRad = 3.14159265358979323846 / 180.0;
        if (yawDeg != 0.0)   cam.yaw   = yawDeg * toRad;
        if (pitchDeg != 0.0) cam.pitch = pitchDeg * toRad;
        mb_camera_frame(&cam, r);       /* re-fit using the new angles */
    }

    if (dist > 0.0) {
        double px = r->size.x * 0.5, py = r->size.y * 0.5, pz = r->size.z * 0.5;
        double dx = px - cam.x, dy = py - cam.y, dz = pz - cam.z;
        double len = sqrt(dx * dx + dy * dy + dz * dz);
        if (len > 1e-6) {
            double k = dist / len;
            cam.x = px - dx * k;
            cam.y = py - dy * k;
            cam.z = pz - dz * k;
        }
    }

    MBSceneOptions opt;
    mb_scene_default(&opt);

    mb_render_scene(arena, &fb, r, &cam, &opt);
    return mb_bmp_write(&fb, path);
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    Arena cliArena;
    mb_arena_init(&cliArena);
    argv_as_utf8(&cliArena, &argc, &argv);
#endif

    /* --- command line ---------------------------------------------------- */
    const char* prompt = NULL;
    const char* shot   = NULL;
    bool preview = false;
    bool tui = false;
    int  shotW = 960, shotH = 600;
    double yawDeg = 0.0, pitchDeg = 0.0, dist = 0.0;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            cli_usage();
            return 0;
        } else if (strcmp(a, "--preview") == 0) {
            preview = true;
        } else if (strcmp(a, "--tui") == 0) {
            tui = true;
        } else if (strcmp(a, "--shot") == 0 && i + 1 < argc) {
            shot = argv[++i];
        } else if (strcmp(a, "--yaw") == 0 && i + 1 < argc) {
            yawDeg = atof(argv[++i]);
        } else if (strcmp(a, "--pitch") == 0 && i + 1 < argc) {
            pitchDeg = atof(argv[++i]);
        } else if (strcmp(a, "--dist") == 0 && i + 1 < argc) {
            dist = atof(argv[++i]);
        } else if (strcmp(a, "--size") == 0 && i + 1 < argc) {
            int w = 0, h = 0;
            if (sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w >= 64 && h >= 64) {
                shotW = w;
                shotH = h;
            }
        } else if (strcmp(a, "--prompt") == 0 && i + 1 < argc) {
            prompt = argv[++i];
        } else if (a[0] != '-') {
            prompt = a;      /* a bare argument is the prompt */
        }
    }

    if (shot || preview) {
        Arena arena;
        mb_arena_init(&arena);

        const char* text = (prompt && *prompt) ? prompt : DEFAULT_PROMPT;
        MBBuildResult* r = cli_generate(&arena, text);
        if (!r) {
            mb_arena_destroy(&arena);
            return 1;
        }

        if (shot) {
            bool ok = cli_write_shot(&arena, r, shot, shotW, shotH,
                                     yawDeg, pitchDeg, dist);
            printf("%s %s  (%dx%d, %s, %d 方块)\n",
                   ok ? "已写出" : "写出失败", shot, shotW, shotH,
                   r->name, (int)r->blockCount);
            mb_arena_destroy(&arena);
            return ok ? 0 : 1;
        }

        char title[256];
        snprintf(title, sizeof(title), "%s · %d×%d×%d",
                 r->name, r->size.x, r->size.y, r->size.z);

        if (!mb_preview_open(r, title, false)) {
            fprintf(stderr, "无法创建预览窗口（此功能需要 Windows 桌面环境）\n");
            mb_arena_destroy(&arena);
            return 1;
        }
        mb_arena_destroy(&arena);
        return 0;
    }

    if (!tui) return mb_gui_run(prompt);

    /* --- terminal interface ---------------------------------------------- */
    Arena frame, model;
    mb_arena_init(&frame);
    mb_arena_init(&model);

    App app;
    memset(&app, 0, sizeof(app));
    snprintf(app.prompt, sizeof(app.prompt), "%s",
             prompt && *prompt ? prompt : DEFAULT_PROMPT);
    mb_llm_default_config(&app.cfg);
    mb_config_load(&app.cfg);
    app.preset = MB_LLM_PRESET_COUNT - 1;
    for (int i = 0; i < MB_LLM_PRESET_COUNT; i++) {
        if (strcmp(app.cfg.baseUrl, MB_LLM_PRESETS[i].baseUrl) == 0
            && strcmp(app.cfg.model, MB_LLM_PRESETS[i].model) == 0) {
            app.preset = i;
            break;
        }
    }
    mb_arena_init(&app.planArena);
    mb_view_default(&app.view);
    app.tab = TAB_3D;

    mb_term_init();

    /* No model call on launch: press Enter when the prompt is ready. */
    snprintf(app.status, sizeof(app.status),
             "在设置页(5)配置模型后按 Enter 生成（需要网络与 API Key）");

    for (;;) {
        int cols, rows;
        mb_term_size(&cols, &rows);
        if (cols < 40) cols = 40;
        if (rows < 14) rows = 14;

        draw(&app, &frame, cols, rows);

        int key = mb_term_read_key();
        if (key == MB_KEY_CTRL_C) break;
        if (key == MB_KEY_NONE || key == MB_KEY_RESIZE) continue;
        if (!app.editing && key >= MB_KEY_CHAR && (key - MB_KEY_CHAR) == 'q') break;

        handle_char(&app, key, &model);
    }

    mb_config_save(&app.cfg);
    mb_term_restore();
    mb_arena_destroy(&app.planArena);
    mb_arena_destroy(&model);
    mb_arena_destroy(&frame);
    return 0;
}
