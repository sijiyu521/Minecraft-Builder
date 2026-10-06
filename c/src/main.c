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

/* ========================================================================== */
/* constants                                                                  */
/* ========================================================================== */

enum { TAB_3D = 0, TAB_PLAN, TAB_REPORT, TAB_EXPORT, TAB_SETTINGS, TAB_COUNT };

static const char* TAB_NAMES[TAB_COUNT] = {
    "3D 预览", "平面图", "报告", "导出", "设置"
};

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

    bool   aiEnabled;
    bool   editing;

    const char* structureId;   /* NULL = let the planner decide */
    const char* styleId;
    MBScaleId   scale;

    int settingsRow;
    int exportRow;

    uint32_t seed;
    MBBuildResult* result;     /* lives in the model arena */
} App;

/* ========================================================================== */
/* text helpers                                                               */
/* ========================================================================== */

static int utf8_cp_len(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static unsigned int utf8_decode(const char* s, int* len)
{
    const unsigned char* p = (const unsigned char*)s;
    int n = utf8_cp_len(p[0]);
    if (n == 1) { *len = 1; return p[0]; }
    unsigned int cp = (unsigned int)(p[0] & (0xFF >> (n + 1)));
    for (int i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) { *len = 1; return p[0]; }
        cp = (cp << 6) | (unsigned int)(p[i] & 0x3F);
    }
    *len = n;
    return cp;
}

/* Display columns, not bytes. CJK is two cells wide, and ANSI escape sequences
   occupy no cells at all — mixed in with the text they must not be counted. */
static int display_width(const char* s)
{
    int w = 0;
    while (*s) {
        if ((unsigned char)*s == 0x1b && s[1] == '[') {
            s += 2;
            while (*s && !((*s >= '@' && *s <= '~'))) s++;
            if (*s) s++;
            continue;
        }
        int n = 0;
        unsigned int cp = utf8_decode(s, &n);
        if (cp < 0x1100)                                  w += 1;
        else if (cp >= 0x1100 && cp <= 0x115F)            w += 2;
        else if (cp >= 0x2E80 && cp <= 0xA4CF)            w += 2;
        else if (cp >= 0xAC00 && cp <= 0xD7A3)            w += 2;
        else if (cp >= 0xF900 && cp <= 0xFAFF)            w += 2;
        else if (cp >= 0xFE30 && cp <= 0xFE6F)            w += 2;
        else if (cp >= 0xFF00 && cp <= 0xFF60)            w += 2;
        else if (cp >= 0xFFE0 && cp <= 0xFFE6)            w += 2;
        else if (cp >= 0x1F300 && cp <= 0x1FAFF)          w += 2;
        else                                              w += 1;
        s += n;
    }
    return w;
}

static void line_pad(SBuf* b, const char* text, int cols)
{
    mb_sb_puts(b, text);
    int w = display_width(text);
    for (int i = w; i < cols - 1; i++) mb_sb_putc(b, ' ');
    mb_sb_puts(b, "\x1b[0m\x1b[K\n");
}

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

static void do_local(App* app, Arena* model)
{
    MBGenerateInput in;
    memset(&in, 0, sizeof(in));
    in.prompt = app->prompt;
    in.structureId = app->structureId;
    in.styleId = app->styleId;
    in.scale = app->scale;
    in.seed = app->seed;

    app->result = mb_generate(model, &in);
}

static void do_ai(App* app, Arena* model)
{
    MBLlmPlanOutcome outcome = mb_llm_plan(model, app->prompt, &app->cfg,
                                           app->structureId, app->styleId);
    if (!outcome.ok) {
        set_error(app, "[%s] %s", mb_llm_error_kind_name(outcome.error.kind),
                  outcome.error.message);
        app->result = NULL;
        return;
    }

    MBLlmTrace trace;
    memset(&trace, 0, sizeof(trace));
    trace.model = outcome.result.model;
    trace.endpoint = outcome.result.endpoint;
    trace.latencyMs = outcome.result.latencyMs;
    trace.reasoning = outcome.result.plan.notes;
    trace.reasoningCount = outcome.result.plan.noteCount;

    app->result = mb_generate_with_plan(model, app->prompt, app->seed,
                                        &outcome.result.plan, &trace,
                                        app->structureId, app->styleId, app->scale);

    snprintf(app->status, sizeof(app->status),
             "模型规划完成：%s · %ld ms", outcome.result.model, outcome.result.latencyMs);
    app->error[0] = '\0';
}

static void regenerate(App* app, Arena* model)
{
    mb_arena_reset(model);
    app->result = NULL;
    app->seed = derive_seed(app);
    app->layer = 0;

    if (app->aiEnabled) {
        do_ai(app, model);
    } else {
        do_local(app, model);
        app->error[0] = '\0';
    }
}

/* ========================================================================== */
/* drawing                                                                    */
/* ========================================================================== */

static void draw_tabs(SBuf* b, const App* app, int cols)
{
    (void)cols;
    for (int i = 0; i < TAB_COUNT; i++) {
        char label[32];
        snprintf(label, sizeof(label), " %d %s ", i + 1, TAB_NAMES[i]);
        if (i == app->tab) mb_sb_puts(b, "\x1b[7m\x1b[1m");
        else               mb_sb_puts(b, "\x1b[0m\x1b[38;2;150;156;172m");
        mb_sb_puts(b, label);
        mb_sb_puts(b, "\x1b[0m ");
    }
    mb_sb_puts(b, "\x1b[0m\x1b[K\n");
}

static void draw_body_3d(SBuf* b, App* app, Arena* frame, int cols, int bodyRows)
{
    const MBBuildResult* r = app->result;
    if (!r) {
        line_pad(b, "  （没有可预览的建筑：请修正上面的错误后按 Enter 重试）", cols);
        for (int i = 1; i < bodyRows; i++) mb_sb_puts(b, "\x1b[K\n");
        return;
    }

    bool withLegend = cols >= 118;
    int isoCols = withLegend ? cols - 46 : cols;

    char* iso = mb_render_iso(frame, r, &app->view, isoCols, bodyRows);
    char* legend = withLegend ? mb_render_legend(frame, r) : NULL;

    /* Split both blocks into lines and interleave them side by side. */
    const char* isoPtr = iso;
    const char* legPtr = legend;
    for (int i = 0; i < bodyRows; i++) {
        const char* isoEnd = strchr(isoPtr, '\n');
        const char* legEnd = legPtr ? strchr(legPtr, '\n') : NULL;

        if (isoEnd) {
            mb_sb_write(b, isoPtr, (size_t)(isoEnd - isoPtr));
        } else {
            for (int k = 0; k < isoCols; k++) mb_sb_putc(b, ' ');
        }

        if (withLegend) {
            mb_sb_puts(b, "\x1b[0m\x1b[38;2;90;96;112m│\x1b[0m ");
            if (legEnd) mb_sb_write(b, legPtr, (size_t)(legEnd - legPtr));
        }
        mb_sb_puts(b, "\x1b[0m\x1b[K\n");

        if (isoEnd) isoPtr = isoEnd + 1;
        if (legEnd) legPtr = legEnd + 1;
        if (!*isoPtr) isoPtr = "";
    }
    (void)app;
}

static void draw_body_plan(SBuf* b, App* app, Arena* frame, int cols, int bodyRows)
{
    const MBBuildResult* r = app->result;
    if (!r) {
        line_pad(b, "  （没有可绘制的建筑）", cols);
        for (int i = 1; i < bodyRows; i++) mb_sb_puts(b, "\x1b[K\n");
        return;
    }

    char head[128];
    snprintf(head, sizeof(head), "  第 %d 层 / 共 %d 层      [ ] 切换楼层",
             app->layer, r->size.y > 0 ? r->size.y - 1 : 0);
    line_pad(b, head, cols);

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
        line_pad(b, "  （没有报告：请修正上面的错误后按 Enter 重试）", cols);
        for (int i = 1; i < bodyRows; i++) mb_sb_puts(b, "\x1b[K\n");
        return;
    }

    const MBPlannerReport* p = &r->report;
    char line[1024];
    int used = 0;

#define RPT(fmt, ...)                                                        \
    do {                                                                     \
        if (used < bodyRows) {                                               \
            snprintf(line, sizeof(line), fmt, __VA_ARGS__);                  \
            line_pad(b, line, cols);                                         \
            used++;                                                          \
        }                                                                    \
    } while (0)

    line_pad(b, "", cols); used++;
    RPT("  %s", r->name);
    RPT("  %s", r->summary);
    line_pad(b, "", cols); used++;
    RPT("  提示词      %s", app->prompt);
    RPT("  建筑类型    %s (%s)", p->structureLabel, p->structure);
    RPT("  体量        %s", p->scale);
    RPT("  种子        %u", (unsigned)p->seed);
    RPT("  识别关键词  %s", join_list(frame, p->keywords, p->keywordCount));
    RPT("  识别材质    %s", join_list(frame, p->detectedMaterials, p->detectedMaterialCount));
    RPT("  修饰语      %s", join_list(frame, p->modifiers, p->modifierCount));
    line_pad(b, "", cols); used++;
    RPT("  尺寸        宽 %d · 高 %d · 深 %d", r->size.x, r->size.y, r->size.z);
    RPT("  方块总数    %d", r->total);
    RPT("  材质种类    %d", r->byMaterialCount);

    if (p->isLlm) {
        line_pad(b, "", cols); used++;
        RPT("  规划引擎    大模型（%s @ %s，%ld ms）",
            p->hasLlmUser ? p->llm.model : "未知",
            p->hasLlmUser ? p->llm.endpoint : "未知",
            p->hasLlmUser ? p->llm.latencyMs : 0L);
        for (int i = 0; i < p->noteCount && used < bodyRows; i++) {
            RPT("  · %s", p->notes[i]);
        }
    } else {
        line_pad(b, "", cols); used++;
        RPT("  规划引擎    %s", "本地规则引擎（确定性，无需联网）");
        for (int i = 0; i < p->noteCount && used < bodyRows; i++) {
            RPT("  · %s", p->notes[i]);
        }
    }
#undef RPT

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
        line_pad(b, "  （没有可导出的建筑）", cols);
        for (int i = 1; i < bodyRows; i++) mb_sb_puts(b, "\x1b[K\n");
        return;
    }

    char base[128];
    export_base_name(app, base, sizeof(base));

    int used = 0;
    line_pad(b, "", cols); used++;
    line_pad(b, "  选中要写出的文件，按 Enter 保存到当前目录：", cols); used++;

    for (int i = 0; i < EX_COUNT && used < bodyRows; i++) {
        char line[512];
        char name[192];
        snprintf(name, sizeof(name), "%s%s", base, EX_NAMES[i]);
        if (i == app->exportRow) {
            snprintf(line, sizeof(line), "  \x1b[7m \xe2\x96\xb6 %-14s %-38s \x1b[0m",
                     EX_NAMES[i], name);
        } else {
            snprintf(line, sizeof(line), "    %-14s %-38s", EX_NAMES[i], name);
        }
        line_pad(b, line, cols);
        used++;

        char desc[256];
        snprintf(desc, sizeof(desc), "      %s", EX_DESCS[i]);
        line_pad(b, desc, cols);
        used++;
    }

    line_pad(b, "", cols); used++;
    if (app->status[0]) {
        char line[1024];
        snprintf(line, sizeof(line), "  已写出：%s", app->status);
        line_pad(b, line, cols);
        used++;
    }

    for (; used < bodyRows; used++) mb_sb_puts(b, "\x1b[K\n");
}

static void draw_body_settings(SBuf* b, App* app, int cols, int bodyRows)
{
    int used = 0;
    line_pad(b, "", cols); used++;
    line_pad(b, "  大模型接入设置（保存后按 1 回到预览，用 a 切换到 AI 规划）", cols); used++;
    line_pad(b, "", cols); used++;

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
            if (app->cfg.apiKey[0]) snprintf(value, sizeof(value), "************（已设置，回车可改）");
            else snprintf(value, sizeof(value), "（未设置）%s",
                          MB_LLM_PRESETS[app->preset].keyOptional ? " — 该服务可留空" : "");
            break;
        case SET_MODEL:
            snprintf(value, sizeof(value), "%s", app->cfg.model);
            break;
        case SET_TEMP:
            snprintf(value, sizeof(value), "%.2f   ← → 微调", app->cfg.temperature);
            break;
        case SET_JSON:
            snprintf(value, sizeof(value), "%s   ← → 切换",
                     app->cfg.jsonMode ? "开启（优先使用 response_format: json_object）" : "关闭");
            break;
        default:
            value[0] = '\0';
        }

        char line[1024];
        if (i == app->settingsRow) {
            snprintf(line, sizeof(line), "  \x1b[7m \xe2\x96\xb6 %-10s %-60s \x1b[0m",
                     SET_NAMES[i], value);
        } else {
            snprintf(line, sizeof(line), "    %-10s %-60s", SET_NAMES[i], value);
        }
        line_pad(b, line, cols);
        used++;
    }

    line_pad(b, "", cols); used++;
    char note[512];
    snprintf(note, sizeof(note), "  提示：%s", MB_LLM_PRESETS[app->preset].hint);
    line_pad(b, note, cols); used++;

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
        char left[256];
        snprintf(left, sizeof(left), " Minecraft Builder · C 版  ");
        char right[512];
        if (app->aiEnabled) {
            snprintf(right, sizeof(right), " 引擎: 大模型 (%s)",
                     app->cfg.model[0] ? app->cfg.model : "未配置");
        } else {
            snprintf(right, sizeof(right), " 引擎: 本地规则引擎");
        }

        char mid[512] = "";
        if (r) {
            snprintf(mid, sizeof(mid), "%s · %s   %d×%d 占地 · 高 %d · %d 方块",
                     r->report.scale, r->name, r->size.x, r->size.z,
                     r->size.y, r->total);
        }

        mb_sb_puts(&b, "\x1b[0m\x1b[48;2;28;32;42m\x1b[1m");
        mb_sb_puts(&b, left);
        mb_sb_puts(&b, "\x1b[22m\x1b[48;2;28;32;42m");
        mb_sb_puts(&b, mid);
        mb_sb_puts(&b, "\x1b[0m\x1b[48;2;28;32;42m\x1b[38;2;130;190;255m");
        mb_sb_puts(&b, right);
        int w = display_width(left) + display_width(mid) + display_width(right);
        for (int i = w; i < cols - 1; i++) mb_sb_putc(&b, ' ');
        mb_sb_puts(&b, "\x1b[0m\x1b[K\n");
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
    if (app->error[0]) {
        char line[1200];
        snprintf(line, sizeof(line), " \x1b[38;2;255;110;110m✗ %s", app->error);
        line_pad(&b, line, cols);
    } else if (app->status[0]) {
        char line[1200];
        snprintf(line, sizeof(line), " \x1b[38;2;120;220;160m✓ %s", app->status);
        line_pad(&b, line, cols);
    } else if (app->editing) {
        char line[1200];
        snprintf(line, sizeof(line), " \x1b[7m提示词\x1b[0m %s\x1b[7m█\x1b[0m", app->prompt);
        line_pad(&b, line, cols);
    } else {
        line_pad(&b, "", cols);
    }

    /* --- hints ----------------------------------------------------------- */
    if (app->editing) {
        line_pad(&b, " 输入中：直接打字 · Backspace 删除 · Enter 生成 · Esc 取消", cols);
    } else if (app->tab == TAB_SETTINGS) {
        line_pad(&b, " ↑↓ 选择字段 · ←→ 调整 · Enter 编辑/切换 · Tab 切换视图 · q 退出", cols);
    } else if (app->tab == TAB_EXPORT) {
        line_pad(&b, " ↑↓ 选择 · Enter 写出文件 · Tab 切换视图 · i 编辑提示词 · q 退出", cols);
    } else {
        line_pad(&b, " i 编辑提示词 · Enter 生成 · a AI/本地 · t 测试连接 · g 换变体 · Tab 切换 · ←→↑↓ 视角 · [ ] 层 · - + 缩放 · q 退出", cols);
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
        case 'a':
            app->aiEnabled = !app->aiEnabled;
            snprintf(app->status, sizeof(app->status), "已切换到%s",
                     app->aiEnabled ? "大模型规划（需要网络与 API Key）" : "本地规则引擎（离线可用）");
            regenerate(app, model);
            break;
        case 'g':
            app->variant++;
            regenerate(app, model);
            snprintf(app->status, sizeof(app->status), "已换一个变体（种子 %u）", (unsigned)app->seed);
            break;
        case 't': {
            Arena scratch;
            mb_arena_init(&scratch);
            test_connection(app, &scratch);
            mb_arena_destroy(&scratch);
            break;
        }
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

int main(void)
{
    Arena frame, model;
    mb_arena_init(&frame);
    mb_arena_init(&model);

    App app;
    memset(&app, 0, sizeof(app));
    snprintf(app.prompt, sizeof(app.prompt), "%s", DEFAULT_PROMPT);
    mb_llm_default_config(&app.cfg);
    mb_view_default(&app.view);
    app.aiEnabled = false;
    app.tab = TAB_3D;
    app.scale = MB_SCALE_AUTO;

    mb_term_init();

    regenerate(&app, &model);

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

    mb_term_restore();
    mb_arena_destroy(&model);
    mb_arena_destroy(&frame);
    return 0;
}
