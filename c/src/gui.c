#include "mb.h"

#include "raylib.h"
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define RAYGUI_IMPLEMENTATION
#include "raygui.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define Rectangle Win32Rectangle
#define CloseWindow Win32CloseWindow
#define ShowCursor Win32ShowCursor
#define DrawTextEx Win32DrawTextEx
#include <windows.h>
#undef DrawTextEx
#undef ShowCursor
#undef CloseWindow
#undef Rectangle
#endif

#define UI_BG       (Color){ 22, 24, 29, 255 }
#define UI_CARD     (Color){ 30, 33, 40, 255 }
#define UI_RAISED   (Color){ 38, 42, 51, 255 }
#define UI_BORDER   (Color){ 51, 56, 69, 255 }
#define UI_TEXT     (Color){ 231, 234, 240, 255 }
#define UI_SOFT     (Color){ 195, 201, 214, 255 }
#define UI_MUTED    (Color){ 139, 147, 167, 255 }
#define UI_GREEN    (Color){ 76, 195, 138, 255 }
#define UI_VIOLET   (Color){ 139, 123, 240, 255 }
#define UI_GOLD     (Color){ 224, 177, 94, 255 }
#define UI_RED      (Color){ 238, 106, 106, 255 }

#define PROMPT_CAP 1024
#define STATUS_CAP 512
#define FONT_CP_CAP 2048

typedef enum { GUI_VIEW_3D, GUI_VIEW_PLAN, GUI_VIEW_REPORT, GUI_VIEW_EXPORT, GUI_VIEW_SETTINGS } GuiTab;
typedef enum { FIELD_NONE, FIELD_PROMPT, FIELD_URL, FIELD_KEY, FIELD_MODEL } GuiField;

typedef struct {
    char prompt[PROMPT_CAP];
    char status[STATUS_CAP];
    MBLlmConfig cfg;
    Arena model;               /* build result */
    Arena plan;                /* cached model plan + trace strings */
    Arena render;              /* framebuffer scratch */
    MBBuildResult* result;
    MBLlmPlan cachedPlan;
    MBLlmTrace cachedTrace;
    bool hasPlan;
    Font font;
    bool hasFont;
    int fontCodepoints[FONT_CP_CAP];
    int fontCodepointCount;
    unsigned char fontSeen[0x10000 / 8];   /* BMP codepoints already requested */
    bool fontDirty;
    Texture2D previewTexture;
    int previewWidth;
    int previewHeight;
    bool sceneDirty;
    MBSceneOptions scene;
    MBCamera camera;
    double pivotX, pivotY, pivotZ, distance;
    int tab;
    int layer;
    int preset;
    int exportRow;
    GuiField field;
    bool promptFocused;
    bool dropdownOpen;
    bool dragging;
    bool inputReady;
    bool inFrame;              /* true between BeginDrawing() and EndDrawing() */
    Vector2 lastMouse;
} GuiApp;

static const char* GUI_TABS[] = { "3D 预览", "楼层图", "生成报告", "导出文件", "模型设置" };
static const char* GUI_PRESETS[] = {
    "DeepSeek", "OpenAI", "Moonshot 月之暗面", "智谱 GLM",
    "通义千问 DashScope", "Ollama 本地", "自定义 / 中转"
};
static const char* GUI_EXPORT_EXT[] = { ".mcfunction", ".json", ".csv", ".md" };
static const char* GUI_EXPORT_NAMES[] = { "Minecraft 函数", "JSON 建筑数据", "CSV 材料清单", "Markdown 报告" };
static const char* GUI_EXPORT_HINTS[] = {
    "在游戏中使用 /function 执行",
    "包含尺寸、坐标和方块 ID",
    "适合表格软件与备料",
    "适合分享或归档"
};

static int color_value(Color c)
{
    return (int)(((unsigned)c.r << 24) | ((unsigned)c.g << 16) | ((unsigned)c.b << 8) | c.a);
}

static void gui_style(void)
{
    GuiSetStyle(DEFAULT, TEXT_SIZE, 18);
    GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL, color_value(UI_SOFT));
    GuiSetStyle(DEFAULT, TEXT_COLOR_FOCUSED, color_value(UI_TEXT));
    GuiSetStyle(DEFAULT, TEXT_COLOR_PRESSED, color_value(UI_BG));
    GuiSetStyle(DEFAULT, BACKGROUND_COLOR, color_value(UI_CARD));
    GuiSetStyle(BUTTON, BORDER_WIDTH, 1);
    GuiSetStyle(BUTTON, BORDER_COLOR_NORMAL, color_value(UI_BORDER));
    GuiSetStyle(BUTTON, BASE_COLOR_NORMAL, color_value(UI_RAISED));
    GuiSetStyle(BUTTON, TEXT_COLOR_NORMAL, color_value(UI_SOFT));
    GuiSetStyle(BUTTON, BORDER_COLOR_FOCUSED, color_value(UI_GREEN));
    GuiSetStyle(BUTTON, BASE_COLOR_FOCUSED, color_value((Color){ 47, 60, 59, 255 }));
    GuiSetStyle(BUTTON, TEXT_COLOR_FOCUSED, color_value(UI_TEXT));
    GuiSetStyle(BUTTON, BORDER_COLOR_PRESSED, color_value(UI_GREEN));
    GuiSetStyle(BUTTON, BASE_COLOR_PRESSED, color_value(UI_GREEN));
    GuiSetStyle(BUTTON, TEXT_COLOR_PRESSED, color_value(UI_BG));
    GuiSetStyle(TOGGLE, BORDER_COLOR_NORMAL, color_value(UI_BORDER));
    GuiSetStyle(TOGGLE, BASE_COLOR_NORMAL, color_value(UI_RAISED));
    GuiSetStyle(TOGGLE, BASE_COLOR_PRESSED, color_value(UI_GREEN));
    GuiSetStyle(TOGGLE, TEXT_COLOR_PRESSED, color_value(UI_BG));
}

static size_t decode_utf8(const unsigned char* s, uint32_t* cp)
{
    if (s[0] < 0x80) { *cp = s[0]; return 1; }
    if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(s[0] & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
        return 2;
    }
    if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6)
            | (uint32_t)(s[2] & 0x3F);
        return 3;
    }
    if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80
        && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(s[0] & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12)
            | ((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F);
        return 4;
    }
    *cp = s[0];
    return 1;
}

static size_t encode_utf8(uint32_t cp, char* out)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static void font_add_text(GuiApp* app, const char* text)
{
    if (!text) return;
    size_t n = strlen(text), i = 0;
    while (i < n) {
        uint32_t cp = 0;
        size_t used = decode_utf8((const unsigned char*)text + i, &cp);
        i += used;
        if (cp < 0x80 || cp == 0x3000) continue;
        bool seen = false;
        if (cp <= 0xFFFF) {
            size_t byte = cp >> 3;
            unsigned char bit = (unsigned char)(1u << (cp & 7u));
            seen = (app->fontSeen[byte] & bit) != 0;
            if (seen || app->fontCodepointCount >= FONT_CP_CAP) continue;
            app->fontSeen[byte] |= bit;
        } else {
            /* Astral-plane glyphs (rare CJK extensions) are not worth a second
             * bitmap; a short linear scan is fine for the handful that appear. */
            for (int k = 0; k < app->fontCodepointCount; k++) {
                if (app->fontCodepoints[k] == (int)cp) { seen = true; break; }
            }
            if (seen || app->fontCodepointCount >= FONT_CP_CAP) continue;
        }
        app->fontCodepoints[app->fontCodepointCount++] = (int)cp;
        app->fontDirty = true;
    }
}

static void font_add_visible_text(GuiApp* app)
{
    static const char* const labels[] = {
        "Minecraft Builder", "桌面建造工作台", "大模型规划", "生成建筑",
        "换个方案", "提示词", "输入建筑描述，Enter 生成", "3D 预览", "楼层图", "生成报告",
        "导出文件", "模型设置", "尺寸", "方块总数", "种材料", "视角", "网格", "坐标轴",
        "自动旋转", "显示网格", "显示坐标轴", "自动环绕", "重置视角", "上一层", "下一层",
        "材料占比", "方案信息", "建筑类型", "风格", "体量", "关键词", "识别材质", "修饰语",
        "规划引擎", "模型", "服务预设", "服务地址", "API 密钥", "温度", "JSON 模式",
        "测试连接", "保存并测试", "选择格式并导出", "已导出", "连接成功", "正在生成...",
        "无建筑", "暂无备注", "大模型", "开", "关", "取消", "AI",
        "生成失败", "请输入 API Key", "当前提示词", "主题",
        "输入 API Key", "点击填入", "点击更改", "Enter 完成",
        "从一句话开始", "场景预览", "拖动旋转 · 滚轮缩放", "场景信息", "实时状态",
        "相机距离", "材料清单", "按用量排序", "生成后显示用量", "楼层平面图",
        "每格代表一个方块", "第 层 / 共 层", "建筑方案", "规划备注", "导出建筑",
        "选择目标文件格式", "在游戏中使用 /function 执行", "包含尺寸、坐标和方块 ID",
        "适合表格软件与备料", "适合分享或归档", "导出到当前目录", "OpenAI 兼容接口",
        "模型名称", "尚未设置", "服务地址", "坐标轴", "材料占比", "暂无数据",
        "请先在模型设置中填写服务地址和模型名", "请先在模型设置中填写 API Key",
        "服务预设已更新", "模型规划完成", "建筑生成失败",
        "请先用大模型生成一次建筑", "用大模型规划建筑", "填写描述后点击生成建筑",
        /* strings that are composed or drawn directly rather than coming from
         * the fixed tables above; without these the built-in font rasterises
         * them as "?" boxes */
        "快捷操作", "例如：海边的日式木屋", "输入建筑描述并生成",
        "方案决策与识别结果", "统计", "保存设置",
        "设置已保存", "设置保存失败",
        "已切换建筑变体 · 种子", "服务连接成功", "先生成一个建筑方案",
        /* punctuation that formatted strings and model output may contain */
        "× · ： ， 。 、 （ ） ？ ！ … “ ” ‘ ’ —"
    };
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) font_add_text(app, labels[i]);
    for (size_t i = 0; i < sizeof(GUI_TABS) / sizeof(GUI_TABS[0]); i++) font_add_text(app, GUI_TABS[i]);
    for (size_t i = 0; i < sizeof(GUI_PRESETS) / sizeof(GUI_PRESETS[0]); i++) font_add_text(app, GUI_PRESETS[i]);
    for (size_t i = 0; i < sizeof(GUI_EXPORT_NAMES) / sizeof(GUI_EXPORT_NAMES[0]); i++) font_add_text(app, GUI_EXPORT_NAMES[i]);
    for (size_t i = 0; i < sizeof(GUI_EXPORT_HINTS) / sizeof(GUI_EXPORT_HINTS[0]); i++) font_add_text(app, GUI_EXPORT_HINTS[i]);
    font_add_text(app, app->prompt);
    if (!app->result) return;
    font_add_text(app, app->result->name);
    font_add_text(app, app->result->summary);
    font_add_text(app, app->result->report.prompt);
    font_add_text(app, app->result->report.structureLabel);
    font_add_text(app, app->result->report.scale);
    for (int i = 0; i < app->result->report.keywordCount; i++) font_add_text(app, app->result->report.keywords[i]);
    for (int i = 0; i < app->result->report.detectedMaterialCount; i++) font_add_text(app, app->result->report.detectedMaterials[i]);
    for (int i = 0; i < app->result->report.modifierCount; i++) font_add_text(app, app->result->report.modifiers[i]);
    for (int i = 0; i < app->result->byMaterialCount; i++) font_add_text(app, mb_material_name(app->result->byMaterial[i].id));
    for (int i = 0; i < app->result->report.noteCount; i++) font_add_text(app, app->result->report.notes[i]);
}

static bool load_ui_font(int* codepoints, int count, Font* out)
{
    /* DengXian is the face the workbench was designed against, but it is part of
     * the optional Chinese font feature on Windows. Fall through the usual
     * system CJK faces so the UI never falls back to "?" boxes just because the
     * machine installed a different language pack. */
    static const char* const candidates[] = {
        "C:/Windows/Fonts/Deng.ttf",
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/msyh.ttf",
        "C:/Windows/Fonts/simhei.ttf",
        "C:/Windows/Fonts/simsun.ttc",
        "C:/Windows/Fonts/msjh.ttc",
        "C:/Windows/Fonts/segoeui.ttf"
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        Font font = LoadFontEx(candidates[i], 20, codepoints, count);
        if (font.texture.id != 0 && font.glyphCount > 1) {
            SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
            *out = font;
            return true;
        }
        if (font.texture.id != 0) UnloadFont(font);
    }
    return false;
}

static void font_reload(GuiApp* app)
{
    if (!app->fontDirty) return;
    /* Swapping textures while a frame is being built would leave already
     * queued text pointing at an unloaded font; defer to the next frame. */
    if (app->inFrame) return;
    app->fontDirty = false;
    Font next;
    if (!load_ui_font(app->fontCodepoints, app->fontCodepointCount, &next)) return;
    GuiSetFont(next);
    if (app->hasFont) UnloadFont(app->font);
    app->font = next;
    app->hasFont = true;
}

/*
 * Any text that reaches draw_text() is a candidate for a glyph that was not in
 * the preload list (a model name with an unusual character, a folder in an
 * export error, a new label added later). Registering it here means the UI can
 * never permanently show "?" boxes; font_reload() picks the additions up on the
 * next frame. The bitmap in GuiApp keeps this cheap enough to run every frame.
 */
static GuiApp* g_fontScan = NULL;

static void draw_text(const char* text, float x, float y, float size, Color color)
{
    if (g_fontScan && text && *text) font_add_text(g_fontScan, text);
    DrawTextEx(GuiGetFont(), text, (Vector2){ x, y }, size, 1.0f, color);
}

static void draw_text_right(const char* text, float right, float y, float size, Color color)
{
    Vector2 measured = MeasureTextEx(GuiGetFont(), text, size, 1.0f);
    draw_text(text, right - measured.x, y, size, color);
}

/* One-line text with an ellipsis when it does not fit. Status messages can
 * carry an HTTP error body and report values can carry a model-authored name,
 * so a plain DrawTextEx here would spill across the card and over the window. */
static void draw_text_clipped(const char* text, float x, float y, float size,
                              float maxWidth, Color color)
{
    if (!text) return;
    if (MeasureTextEx(GuiGetFont(), text, size, 1.0f).x <= maxWidth) {
        draw_text(text, x, y, size, color);
        return;
    }

    char clipped[STATUS_CAP + 8];
    size_t len = strlen(text);
    if (len >= sizeof(clipped) - 4) len = sizeof(clipped) - 4;
    memcpy(clipped, text, len);
    clipped[len] = '\0';

    static const char* const ellipsis = "…";
    float ellipsisWidth = MeasureTextEx(GuiGetFont(), ellipsis, size, 1.0f).x;
    while (len > 0) {
        len--;
        while (len > 0 && ((unsigned char)clipped[len] & 0xC0) == 0x80) len--;
        clipped[len] = '\0';
        if (MeasureTextEx(GuiGetFont(), clipped, size, 1.0f).x + ellipsisWidth <= maxWidth) break;
    }
    strncat(clipped, ellipsis, sizeof(clipped) - strlen(clipped) - 1);
    draw_text(clipped, x, y, size, color);
}

static const char* prompt_visible_suffix(const char* text, float maxWidth, float size)
{
    const char* start = text;
    while (*start && MeasureTextEx(GuiGetFont(), start, size, 1.0f).x > maxWidth) {
        uint32_t cp = 0;
        size_t used = decode_utf8((const unsigned char*)start, &cp);
        start += used;
    }
    return start;
}

static void draw_card(Rectangle bounds, const char* title, const char* eyebrow)
{
    DrawRectangleRounded(bounds, 0.08f, 8, UI_CARD);
    DrawRectangleRoundedLinesEx(bounds, 0.08f, 8, 1.0f, UI_BORDER);
    if (eyebrow) draw_text(eyebrow, bounds.x + 18, bounds.y + 15, 12, UI_GREEN);
    if (title) draw_text(title, bounds.x + 18, bounds.y + (eyebrow ? 34 : 17), 20, UI_TEXT);
}

static void set_status(GuiApp* app, const char* text)
{
    if (!text) text = "";
    size_t n = strlen(text);
    if (n >= sizeof(app->status)) n = sizeof(app->status) - 1;
    memcpy(app->status, text, n);
    app->status[n] = '\0';
    font_add_text(app, app->status);
    font_reload(app);
}

static void utf8_backspace(char* text)
{
    size_t n = strlen(text);
    if (!n) return;
    n--;
    while (n > 0 && (((unsigned char)text[n] & 0xC0) == 0x80)) n--;
    text[n] = '\0';
}

static void input_codepoints(GuiApp* app, char* destination, size_t capacity)
{
    if (app->field == FIELD_NONE || !destination || capacity == 0) return;
    int cp;
    while ((cp = GetCharPressed()) != 0) {
        if (cp < 32 || cp == 127) continue;
        char encoded[4];
        size_t bytes = encode_utf8((uint32_t)cp, encoded);
        if ((uint32_t)cp >= 0x80) {
            char glyph[5] = { 0 };
            memcpy(glyph, encoded, bytes);
            font_add_text(app, glyph);
        }
        size_t len = strlen(destination);
        if (len + bytes >= capacity) continue;
        memcpy(destination + len, encoded, bytes);
        destination[len + bytes] = '\0';
    }

    /* Pasting is the normal way to enter a base URL or API key, and raylib only
     * reports typed characters through GetCharPressed(). */
    bool paste = (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL))
                 && IsKeyPressed(KEY_V);
    if (paste) {
        const char* clip = GetClipboardText();
        if (clip) {
            size_t len = strlen(destination);
            for (const unsigned char* p = (const unsigned char*)clip; *p; ) {
                uint32_t ch = 0;
                size_t used = decode_utf8(p, &ch);
                p += used;
                if (ch < 32 || ch == 127) continue;    /* strip line breaks */
                char encoded[4];
                size_t bytes = encode_utf8(ch, encoded);
                if (len + bytes >= capacity) break;
                memcpy(destination + len, encoded, bytes);
                len += bytes;
            }
            destination[len] = '\0';
            font_add_text(app, destination);
        }
    }

    if (IsKeyPressed(KEY_BACKSPACE)) utf8_backspace(destination);
    font_reload(app);
}

static void camera_from_orbit(GuiApp* app)
{
    double cp = cos(app->camera.pitch);
    app->camera.x = app->pivotX - cp * sin(app->camera.yaw) * app->distance;
    app->camera.y = app->pivotY - sin(app->camera.pitch) * app->distance;
    app->camera.z = app->pivotZ - cp * cos(app->camera.yaw) * app->distance;
}

static void frame_camera(GuiApp* app)
{
    if (!app->result) return;
    mb_camera_default(&app->camera, app->result);
    app->pivotX = app->result->size.x * 0.5;
    app->pivotY = app->result->size.y * 0.5;
    app->pivotZ = app->result->size.z * 0.5;
    double dx = app->pivotX - app->camera.x;
    double dy = app->pivotY - app->camera.y;
    double dz = app->pivotZ - app->camera.z;
    app->distance = sqrt(dx * dx + dy * dy + dz * dz);
    app->sceneDirty = true;
}

/* A configured endpoint is now mandatory: generation is model-driven only.
 * Checking here turns a first-run HTTP 401 into an actionable message. */
static bool model_config_ready(GuiApp* app)
{
    if (!app->cfg.baseUrl[0] || !app->cfg.model[0]) {
        set_status(app, "请先在模型设置中填写服务地址和模型名");
        return false;
    }
    bool keyOptional = app->preset >= 0 && app->preset < MB_LLM_PRESET_COUNT
                       && MB_LLM_PRESETS[app->preset].keyOptional;
    if (!app->cfg.apiKey[0] && !keyOptional) {
        set_status(app, "请先在模型设置中填写 API Key");
        return false;
    }
    return true;
}

/* The trace is kept for "换个方案": it has to outlive the planning call, so
 * its strings are copied into the plan arena. */
static void cache_trace(GuiApp* app, const MBLlmPlanOutcome* outcome)
{
    memset(&app->cachedTrace, 0, sizeof(app->cachedTrace));
    app->cachedTrace.model = mb_arena_strdup(&app->plan, outcome->result.model);
    app->cachedTrace.endpoint = mb_arena_strdup(&app->plan, outcome->result.endpoint);
    app->cachedTrace.latencyMs = outcome->result.latencyMs;
    app->cachedTrace.reasoning = app->cachedPlan.notes;
    app->cachedTrace.reasoningCount = app->cachedPlan.noteCount;
}

static void regenerate(GuiApp* app)
{
    mb_arena_reset(&app->model);
    mb_arena_reset(&app->plan);
    app->result = NULL;
    app->hasPlan = false;
    app->layer = 0;
    if (!model_config_ready(app)) return;

    MBLlmPlanOutcome outcome = mb_llm_plan(&app->plan, app->prompt, &app->cfg, NULL, NULL);
    if (!outcome.ok) {
        set_status(app, outcome.error.message);
        return;
    }

    app->cachedPlan = outcome.result.plan;
    cache_trace(app, &outcome);
    app->hasPlan = true;

    app->result = mb_generate(&app->model, app->prompt, mb_hash_string(app->prompt),
                              &app->cachedPlan, &app->cachedTrace);
    if (!app->result) {
        set_status(app, "建筑生成失败");
        return;
    }

    char msg[STATUS_CAP];
    snprintf(msg, sizeof(msg), "模型规划完成 · %s · %ld ms",
             outcome.result.model, outcome.result.latencyMs);
    set_status(app, msg);
    frame_camera(app);
    font_add_visible_text(app);
    font_reload(app);
}

static void regenerate_variant(GuiApp* app)
{
    if (!app->hasPlan) {
        set_status(app, "请先用大模型生成一次建筑");
        return;
    }

    /* Reuse the cached intent and only re-roll the seed: no extra tokens. */
    static unsigned variant = 1;
    ++variant;
    mb_arena_reset(&app->model);
    app->result = NULL;
    app->layer = 0;
    uint32_t seed = mb_hash_string(app->prompt) + variant * 7919u;
    app->result = mb_generate(&app->model, app->prompt, seed,
                              &app->cachedPlan, &app->cachedTrace);
    if (!app->result) {
        set_status(app, "建筑生成失败");
        return;
    }

    char msg[STATUS_CAP];
    snprintf(msg, sizeof(msg), "已切换建筑变体 · 种子 %u", (unsigned)seed);
    set_status(app, msg);
    frame_camera(app);
    font_add_visible_text(app);
    font_reload(app);
}

static void update_preview_texture(GuiApp* app, int targetW, int targetH)
{
    if (!app->result || targetW < 64 || targetH < 64) return;
    int w = targetW > 960 ? 960 : targetW;
    int h = targetH > 620 ? 620 : targetH;
    if (!app->sceneDirty && app->previewTexture.id && app->previewWidth == w && app->previewHeight == h) return;

    mb_arena_reset(&app->render);
    MBFramebuffer fb;
    mb_fb_init(&fb, &app->render, w, h);
    mb_render_scene(&app->render, &fb, app->result, &app->camera, &app->scene);
    uint32_t* rgba = (uint32_t*)mb_arena_alloc(&app->render, (size_t)w * (size_t)h * sizeof(uint32_t));
    if (!rgba) return;
    size_t count = (size_t)w * (size_t)h;
    for (size_t i = 0; i < count; i++) {
        uint32_t c = fb.color[i];
        rgba[i] = 0xFF000000u | ((c & 0x0000FFu) << 16)
                | (c & 0x00FF00u) | ((c >> 16) & 0x0000FFu);
    }

    if (!app->previewTexture.id || app->previewWidth != w || app->previewHeight != h) {
        if (app->previewTexture.id) UnloadTexture(app->previewTexture);
        Image image = { 0 };
        image.data = rgba;
        image.width = w;
        image.height = h;
        image.mipmaps = 1;
        image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        app->previewTexture = LoadTextureFromImage(image);
        app->previewWidth = w;
        app->previewHeight = h;
    } else {
        UpdateTexture(app->previewTexture, rgba);
    }
    app->sceneDirty = false;
}

static bool save_file(const char* path, const char* text)
{
    FILE* file = fopen(path, "wb");
    if (!file) return false;
    size_t len = strlen(text);
    bool ok = fwrite(text, 1, len, file) == len;
    fclose(file);
    return ok;
}

static void export_selected(GuiApp* app)
{
    if (!app->result) { set_status(app, "先生成一个建筑方案"); return; }
    Arena temp;
    mb_arena_init(&temp);
    char* text = NULL;
    const char* ext = GUI_EXPORT_EXT[app->exportRow];
    switch (app->exportRow) {
    case 0: text = mb_export_mcfunction(&temp, app->result, "builder"); break;
    case 1: text = mb_export_json(&temp, app->result); break;
    case 2: text = mb_export_csv(&temp, app->result); break;
    default: text = mb_export_markdown(&temp, app->result); break;
    }
    char* slug = mb_slugify(&temp, app->result->name);
    char path[256];
    snprintf(path, sizeof(path), "%s%s", slug ? slug : "building", ext);
    bool ok = text && save_file(path, text);
    char msg[STATUS_CAP];
    snprintf(msg, sizeof(msg), ok ? "已导出到 %s" : "写入 %s 失败", path);
    set_status(app, msg);
    mb_arena_destroy(&temp);
}

static void test_connection(GuiApp* app)
{
    if (!app->cfg.baseUrl[0] || !app->cfg.model[0]) {
        set_status(app, "请先填写服务地址和模型名");
        return;
    }
    Arena temp;
    mb_arena_init(&temp);
    MBLlmPlanOutcome outcome = mb_llm_ping(&temp, &app->cfg);
    if (!outcome.ok) set_status(app, outcome.error.message);
    else set_status(app, outcome.result.raw ? outcome.result.raw : "服务连接成功");
    mb_arena_destroy(&temp);
}

static void apply_preset(GuiApp* app, int delta)
{
    app->preset = (app->preset + delta + MB_LLM_PRESET_COUNT) % MB_LLM_PRESET_COUNT;
    snprintf(app->cfg.baseUrl, sizeof(app->cfg.baseUrl), "%s",
             MB_LLM_PRESETS[app->preset].baseUrl);
    snprintf(app->cfg.model, sizeof(app->cfg.model), "%s",
             MB_LLM_PRESETS[app->preset].model);
    set_status(app, "服务预设已更新");
}

static void draw_topbar(GuiApp* app, int width)
{
    DrawRectangle(0, 0, width, 82, UI_CARD);
    DrawRectangle(0, 80, width, 2, UI_BORDER);
    DrawRectangle(0, 0, 4, 82, UI_GREEN);
    draw_text("MB", 24, 18, 28, UI_GREEN);
    draw_text("Minecraft Builder", 72, 14, 22, UI_TEXT);
    draw_text("桌面建造工作台", 73, 45, 14, UI_MUTED);
    if (app->result) {
        char summary[160];
        snprintf(summary, sizeof(summary), "%d × %d × %d     %d 方块",
                 app->result->size.x, app->result->size.y, app->result->size.z,
                 app->result->total);
        draw_text_right(summary, (float)width - 190, 29, 16, UI_SOFT);
    }
    Rectangle engine = { (float)width - 176, 20, 150, 34 };
    DrawRectangleRounded(engine, 0.45f, 8, UI_VIOLET);
    const char* mode = "大模型规划";
    Vector2 m = MeasureTextEx(GuiGetFont(), mode, 14, 1.0f);
    draw_text(mode, engine.x + (engine.width - m.x) * 0.5f, engine.y + 8, 14, UI_TEXT);
}

static Rectangle draw_sidebar(GuiApp* app, int height)
{
    Rectangle panel = { 20, 102, 292, (float)height - 122 };
    draw_card(panel, "建筑描述", "从一句话开始");

    Rectangle prompt = { panel.x + 16, panel.y + 82, panel.width - 32, 112 };
    bool promptHover = CheckCollisionPointRec(GetMousePosition(), prompt);
    if (app->inputReady && promptHover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) app->field = FIELD_PROMPT;
    DrawRectangleRounded(prompt, 0.08f, 8, app->field == FIELD_PROMPT ? UI_RAISED : UI_BG);
    DrawRectangleRoundedLinesEx(prompt, 0.08f, 8, 1.0f,
                                app->field == FIELD_PROMPT ? UI_GREEN : UI_BORDER);
    if (!app->prompt[0] && app->field != FIELD_PROMPT) {
        draw_text("例如：海边的日式木屋", prompt.x + 12, prompt.y + 12, 14, UI_MUTED);
    } else {
        const char* visible = prompt_visible_suffix(app->prompt, prompt.width - 24, 16);
        Vector2 pos = { prompt.x + 12, prompt.y + 12 };
        DrawTextEx(GuiGetFont(), visible, pos, 16, 1.0f, UI_SOFT);
        if (app->field == FIELD_PROMPT && ((int)(GetTime() * 2.0) % 2 == 0)) {
            Vector2 measured = MeasureTextEx(GuiGetFont(), visible, 16, 1.0f);
            DrawRectangle((int)(pos.x + measured.x + 1), (int)pos.y + 2, 2, 20, UI_GREEN);
        }
    }

    Rectangle generate = { panel.x + 16, panel.y + 210, panel.width - 32, 44 };
    if (GuiButton(generate, "生成建筑")) {
        app->field = FIELD_NONE;
        regenerate(app);
    }
    Rectangle actions = { panel.x + 16, panel.y + 264, (panel.width - 40) * 0.5f, 38 };
    if (GuiButton(actions, "换个方案")) regenerate_variant(app);
    actions.x += actions.width + 8;
    if (GuiButton(actions, "模型设置")) {
        app->tab = GUI_VIEW_SETTINGS;
        app->field = FIELD_NONE;
    }

    float keyY = panel.y + 326;
    DrawLineEx((Vector2){ panel.x + 16, keyY }, (Vector2){ panel.x + panel.width - 16, keyY }, 1, UI_BORDER);
    draw_text("快捷操作", panel.x + 17, keyY + 14, 13, UI_MUTED);
    draw_text("Enter 生成   Esc 取消输入", panel.x + 17, keyY + 38, 13, UI_SOFT);
    draw_text("拖动预览旋转 · 滚轮缩放", panel.x + 17, keyY + 62, 13, UI_SOFT);

    float statusY = panel.y + panel.height - 74;
    DrawRectangleRounded((Rectangle){ panel.x + 12, statusY, panel.width - 24, 60 }, 0.08f, 8,
                         app->status[0] ? (app->result ? UI_RAISED : (Color){ 67, 38, 43, 255 }) : UI_BG);
    Color statusColor = app->result ? UI_SOFT : UI_RED;
    draw_text_clipped(app->status[0] ? app->status : "用大模型规划建筑",
                      panel.x + 22, statusY + 13, 13, panel.width - 44, statusColor);
    return panel;
}

static void draw_materials(GuiApp* app, Rectangle panel)
{
    draw_card(panel, "材料清单", "按用量排序");
    if (!app->result) {
        draw_text("生成后显示用量", panel.x + 18, panel.y + 76, 14, UI_MUTED);
        return;
    }
    int visibleRows = (int)((panel.height - 98.0f) / 48.0f);
    if (visibleRows < 1) visibleRows = 1;
    if (visibleRows > 10) visibleRows = 10;
    int limit = app->result->byMaterialCount < visibleRows
        ? app->result->byMaterialCount : visibleRows;
    float y = panel.y + 78;
    float barW = panel.width - 106;
    for (int i = 0; i < limit; i++) {
        const MBMaterialUsage* u = &app->result->byMaterial[i];
        const MBMaterial* material = mb_material(u->id);
        float pct = app->result->total > 0 ? (float)u->count / (float)app->result->total : 0.0f;
        char count[24];
        snprintf(count, sizeof(count), "%d", u->count);
        DrawRectangleRounded((Rectangle){ panel.x + 16, y + 5, 10, 10 }, 0.25f, 4,
                             (Color){ (unsigned char)(material->color >> 16),
                                      (unsigned char)(material->color >> 8),
                                      (unsigned char)material->color, 255 });
        draw_text(material->name, panel.x + 34, y + 1, 13, UI_SOFT);
        draw_text_right(count, panel.x + panel.width - 16, y + 1, 13, UI_TEXT);
        DrawRectangleRounded((Rectangle){ panel.x + 34, y + 23, barW, 5 }, 0.5f, 6, UI_BG);
        DrawRectangleRounded((Rectangle){ panel.x + 34, y + 23,
            fmaxf(3.0f, barW * pct), 5 }, 0.5f, 6,
            (Color){ (unsigned char)(material->color >> 16),
                     (unsigned char)(material->color >> 8),
                     (unsigned char)material->color, 255 });
        y += 48;
    }
    if (app->result->byMaterialCount > limit) {
        char more[64];
        snprintf(more, sizeof(more), "+%d 种材料", app->result->byMaterialCount - limit);
        draw_text(more, panel.x + 18, y, 13, UI_MUTED);
    }
}

static Rectangle central_bounds(int width, int height, bool hasRightPanel)
{
    float reserved = hasRightPanel ? 648.0f : 352.0f;
    return (Rectangle){ 332, 146, (float)width - reserved, (float)height - 190 };
}

static void draw_preview(GuiApp* app, Rectangle bounds)
{
    draw_card(bounds, "场景预览", "拖动旋转 · 滚轮缩放");
    Rectangle view = { bounds.x + 12, bounds.y + 62, bounds.width - 24, bounds.height - 108 };
    DrawRectangleRounded(view, 0.06f, 8, UI_BG);

    if (!app->result) {
        draw_text("填写描述后点击生成建筑", view.x + 24, view.y + 24, 18, UI_MUTED);
        return;
    }

    update_preview_texture(app, (int)view.width, (int)view.height);
    if (app->previewTexture.id) {
        DrawTexturePro(app->previewTexture,
            (Rectangle){ 0, 0, (float)app->previewTexture.width, (float)app->previewTexture.height },
            view, (Vector2){ 0, 0 }, 0.0f, WHITE);
    }

    Vector2 mouse = GetMousePosition();
    if (app->inputReady && CheckCollisionPointRec(mouse, view) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        app->dragging = true;
        app->lastMouse = mouse;
    }
    if (app->inputReady && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) app->dragging = false;
    if (app->dragging && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        Vector2 now = GetMousePosition();
        app->camera.yaw += (double)(now.x - app->lastMouse.x) * 0.008;
        app->camera.pitch -= (double)(now.y - app->lastMouse.y) * 0.006;
        if (app->camera.pitch > 1.35) app->camera.pitch = 1.35;
        if (app->camera.pitch < -1.35) app->camera.pitch = -1.35;
        app->lastMouse = now;
        camera_from_orbit(app);
        app->sceneDirty = true;
    }
    if (app->inputReady && CheckCollisionPointRec(mouse, view)) {
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            app->distance *= wheel > 0 ? 0.88 : 1.14;
            if (app->distance < 2.0) app->distance = 2.0;
            if (app->distance > 300.0) app->distance = 300.0;
            camera_from_orbit(app);
            app->sceneDirty = true;
        }
    }

    Rectangle controls = { bounds.x + 18, bounds.y + bounds.height - 38, 74, 26 };
    if (GuiToggle(controls, "网格", &app->scene.showGrid)) app->sceneDirty = true;
    controls.x += 80;
    controls.width = 88;
    if (GuiToggle(controls, "坐标轴", &app->scene.showAxes)) app->sceneDirty = true;
    controls.x += 94;
    controls.width = 98;
    if (GuiToggle(controls, "自动环绕", &app->scene.autoOrbit)) app->sceneDirty = true;
    controls.x += 104;
    controls.width = 82;
    if (GuiButton(controls, "重置视角")) frame_camera(app);
    if (app->scene.autoOrbit) {
        app->camera.yaw += GetFrameTime() * 0.25;
        camera_from_orbit(app);
        app->sceneDirty = true;
    }
}

static void draw_plan(GuiApp* app, Rectangle bounds)
{
    draw_card(bounds, "楼层平面图", "每格代表一个方块");
    if (!app->result) return;

    int levels = app->result->size.y;
    Rectangle prev = { bounds.x + 18, bounds.y + 65, 104, 34 };
    Rectangle next = { bounds.x + 130, bounds.y + 65, 104, 34 };
    if (GuiButton(prev, "上一层") && app->layer > 0) app->layer--;
    if (GuiButton(next, "下一层") && app->layer < levels - 1) app->layer++;
    char label[80];
    snprintf(label, sizeof(label), "第 %d 层 / 共 %d 层", app->layer + 1, levels);
    draw_text(label, bounds.x + 252, bounds.y + 72, 15, UI_SOFT);

    float cell = fminf((bounds.width - 64) / (float)app->result->size.x,
                       (bounds.height - 140) / (float)app->result->size.z);
    if (cell > 24) cell = 24;
    if (cell < 4) cell = 4;
    float gridW = cell * (float)app->result->size.x;
    float gridH = cell * (float)app->result->size.z;
    float x0 = bounds.x + (bounds.width - gridW) * 0.5f;
    float y0 = bounds.y + 118 + fmaxf(0.0f, (bounds.height - 140 - gridH) * 0.5f);

    for (size_t i = 0; i < app->result->blockCount; i++) {
        const MBVoxel* block = &app->result->blocks[i];
        if (block->y != app->layer) continue;
        uint32_t c = mb_material_color(block->material);
        Color color = { (unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c, 255 };
        DrawRectangleRec((Rectangle){ x0 + block->x * cell, y0 + block->z * cell,
                                      fmaxf(1.0f, cell - 1.0f), fmaxf(1.0f, cell - 1.0f) }, color);
    }
}

static void report_line(const char* label, const char* value, float x, float* y, float width)
{
    draw_text(label, x, *y, 14, UI_MUTED);
    draw_text_clipped(value ? value : "无", x + 130, *y, 15, width - 130, UI_SOFT);
    *y += 34;
    DrawLineEx((Vector2){ x, *y - 8 }, (Vector2){ x + width, *y - 8 }, 1.0f, UI_BORDER);
}

static void draw_report(GuiApp* app, Rectangle bounds)
{
    draw_card(bounds, "生成报告", "方案决策与识别结果");
    if (!app->result) return;
    MBPlannerReport* report = &app->result->report;
    float x = bounds.x + 24, y = bounds.y + 78;
    float width = bounds.width - 48;
    report_line("建筑方案", app->result->name, x, &y, width);
    report_line("建筑类型", report->structureLabel, x, &y, width);
    report_line("体量", report->scale, x, &y, width);
    char size[96];
    snprintf(size, sizeof(size), "%d × %d × %d", app->result->size.x,
             app->result->size.y, app->result->size.z);
    report_line("尺寸", size, x, &y, width);
    char blocks[48];
    snprintf(blocks, sizeof(blocks), "%d 方块 · %d 种材料", app->result->total,
             app->result->byMaterialCount);
    report_line("统计", blocks, x, &y, width);
    report_line("规划引擎", "大模型", x, &y, width);
    report_line("关键词", report->keywordCount ? report->keywords[0] : "无", x, &y, width);
    if (report->noteCount > 0) {
        draw_text("规划备注", x, y + 8, 14, UI_GREEN);
        draw_text_clipped(report->notes[0], x, y + 34, 14, width, UI_SOFT);
    }
}

static void draw_export(GuiApp* app, Rectangle bounds)
{
    draw_card(bounds, "导出建筑", "选择目标文件格式");
    for (int i = 0; i < 4; i++) {
        float y = bounds.y + 78 + (float)i * 74;
        Rectangle row = { bounds.x + 18, y, bounds.width - 36, 60 };
        DrawRectangleRounded(row, 0.08f, 8, app->exportRow == i ? UI_RAISED : UI_BG);
        DrawRectangleRoundedLinesEx(row, 0.08f, 8, 1.0f,
                                    app->exportRow == i ? UI_GREEN : UI_BORDER);
        draw_text(GUI_EXPORT_NAMES[i], row.x + 14, row.y + 8, 15, UI_TEXT);
        draw_text(GUI_EXPORT_HINTS[i], row.x + 14, row.y + 32, 12, UI_MUTED);
        draw_text_right(GUI_EXPORT_EXT[i], row.x + row.width - 12, row.y + 18, 14, UI_GREEN);
        if (app->inputReady && CheckCollisionPointRec(GetMousePosition(), row) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            app->exportRow = i;
    }
    Rectangle button = { bounds.x + 18, bounds.y + 390, 180, 42 };
    if (GuiButton(button, "导出到当前目录")) export_selected(app);
}

static void draw_settings(GuiApp* app, Rectangle bounds)
{
    draw_card(bounds, "模型设置", "OpenAI 兼容接口");
    float x = bounds.x + 22, y = bounds.y + 78;
    draw_text("服务预设", x, y + 5, 14, UI_MUTED);
    Rectangle prev = { x + 138, y, 38, 32 };
    Rectangle next = { x + 180, y, 38, 32 };
    if (GuiButton(prev, "<")) apply_preset(app, -1);
    if (GuiButton(next, ">")) apply_preset(app, 1);
    draw_text(GUI_PRESETS[app->preset < 7 ? app->preset : 6], x + 230, y + 5, 14, UI_TEXT);

    const char* labels[] = { "服务地址", "模型名称", "API 密钥" };
    char* values[] = { app->cfg.baseUrl, app->cfg.model, app->cfg.apiKey };
    const size_t caps[] = { sizeof(app->cfg.baseUrl), sizeof(app->cfg.model), sizeof(app->cfg.apiKey) };
    const GuiField fields[] = { FIELD_URL, FIELD_MODEL, FIELD_KEY };
    for (int i = 0; i < 3; i++) {
        float rowY = y + 54 + (float)i * 74;
        draw_text(labels[i], x, rowY, 13, UI_MUTED);
        Rectangle input = { x, rowY + 22, bounds.width - 44, 38 };
        bool hovered = CheckCollisionPointRec(GetMousePosition(), input);
        if (app->inputReady && hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) app->field = fields[i];
        DrawRectangleRounded(input, 0.08f, 8, app->field == fields[i] ? UI_RAISED : UI_BG);
        DrawRectangleRoundedLinesEx(input, 0.08f, 8, 1.0f,
                                    app->field == fields[i] ? UI_GREEN : UI_BORDER);
        if (i == 2) {
            size_t keyLength = strlen(values[i]);
            size_t visibleLength = keyLength < 32 ? keyLength : 32;
            char masked[40];
            memset(masked, '*', visibleLength);
            masked[visibleLength] = '\0';
            const char* shown = masked;
            if (!keyLength) {
                shown = app->field == FIELD_KEY ? "输入 API Key" : "尚未设置 · 点击填入";
            }
            draw_text(shown, input.x + 12, input.y + 8, 14,
                      keyLength || app->field == FIELD_KEY ? UI_SOFT : UI_MUTED);
            if (app->field == FIELD_KEY && ((int)(GetTime() * 2.0) % 2 == 0)) {
                Vector2 measured = MeasureTextEx(GuiGetFont(), shown, 14, 1.0f);
                DrawRectangle((int)(input.x + 13 + measured.x), (int)input.y + 9, 2, 19, UI_GREEN);
            }
        } else {
            const char* shown = values[i];
            char withCursor[sizeof(app->cfg.baseUrl) + 2];
            if (app->field == fields[i] && ((int)(GetTime() * 2.0) % 2 == 0)) {
                size_t len = strlen(values[i]);
                if (len >= sizeof(withCursor) - 2) len = sizeof(withCursor) - 2;
                memcpy(withCursor, values[i], len);
                withCursor[len++] = '|';
                withCursor[len] = '\0';
                shown = withCursor;
            }
            draw_text(shown, input.x + 12, input.y + 8, 14, UI_SOFT);
        }
        if (app->field == fields[i] && app->field != FIELD_KEY && hovered) app->field = fields[i];
        if (app->field == fields[i] && IsKeyPressed(KEY_ENTER)) app->field = FIELD_NONE;
        if (app->field == fields[i] && IsKeyPressed(KEY_ESCAPE)) app->field = FIELD_NONE;
        if (app->field == fields[i]) input_codepoints(app, values[i], caps[i]);
    }

    float sliderY = y + 296;
    draw_text("温度", x, sliderY + 4, 14, UI_MUTED);
    char temp[24];
    snprintf(temp, sizeof temp, "%.1f", app->cfg.temperature);
    float t = (float)app->cfg.temperature;
    GuiSlider((Rectangle){ x + 80, sliderY, bounds.width - 170, 24 }, "", temp, &t, 0.0f, 2.0f);
    app->cfg.temperature = t;
    bool json = app->cfg.jsonMode;
    if (GuiCheckBox((Rectangle){ x, sliderY + 42, 20, 20 }, "JSON 模式", &json)) app->cfg.jsonMode = json;
    Rectangle ping = { x, sliderY + 88, 154, 40 };
    if (GuiButton(ping, "测试连接")) test_connection(app);
    Rectangle save = { x + 166, sliderY + 88, 140, 40 };
    if (GuiButton(save, "保存设置")) {
        set_status(app, mb_config_save(&app->cfg) ? "设置已保存" : "设置保存失败");
    }
}

static void draw_right_panel(GuiApp* app, int width, int height)
{
    Rectangle panel = { (float)width - 298, 146, 278, (float)height - 166 };
    if (app->tab == GUI_VIEW_3D || app->tab == GUI_VIEW_PLAN) draw_materials(app, panel);
}

static void add_key_input(GuiApp* app)
{
    if (!app->inputReady) return;
    if (app->field == FIELD_PROMPT) input_codepoints(app, app->prompt, sizeof(app->prompt));
    if (app->field == FIELD_PROMPT && IsKeyPressed(KEY_ENTER)) {
        app->field = FIELD_NONE;
        regenerate(app);
    } else if (IsKeyPressed(KEY_ESCAPE)) {
        app->field = FIELD_NONE;
    } else if (app->tab == GUI_VIEW_3D) {
        if (IsKeyPressed(KEY_LEFT)) { app->camera.yaw -= 0.12; camera_from_orbit(app); app->sceneDirty = true; }
        if (IsKeyPressed(KEY_RIGHT)) { app->camera.yaw += 0.12; camera_from_orbit(app); app->sceneDirty = true; }
        if (IsKeyPressed(KEY_UP)) { app->camera.pitch += 0.06; camera_from_orbit(app); app->sceneDirty = true; }
        if (IsKeyPressed(KEY_DOWN)) { app->camera.pitch -= 0.06; camera_from_orbit(app); app->sceneDirty = true; }
    }
}

/* Keep the preset label in sync with what was loaded from disk: an endpoint
 * typed by hand (or a config file from an older run) lands on "自定义". */
static void sync_preset(GuiApp* app)
{
    app->preset = MB_LLM_PRESET_COUNT - 1;
    for (int i = 0; i < MB_LLM_PRESET_COUNT; i++) {
        if (strcmp(app->cfg.baseUrl, MB_LLM_PRESETS[i].baseUrl) == 0
            && strcmp(app->cfg.model, MB_LLM_PRESETS[i].model) == 0) {
            app->preset = i;
            break;
        }
    }
}

int mb_gui_run(const char* initialPrompt)
{
#ifdef _WIN32
    if (GetConsoleWindow()) FreeConsole();
#endif
    GuiApp app;
    memset(&app, 0, sizeof(app));
    g_fontScan = &app;
    snprintf(app.prompt, sizeof(app.prompt), "%s",
             initialPrompt && *initialPrompt ? initialPrompt :
             "现代风格两层别墅，白墙落地窗，尺寸 15x11");
    mb_arena_init(&app.model);
    mb_arena_init(&app.plan);
    mb_arena_init(&app.render);
    mb_llm_default_config(&app.cfg);
    mb_config_load(&app.cfg);
    sync_preset(&app);
    mb_scene_default(&app.scene);
    app.tab = GUI_VIEW_3D;
    app.sceneDirty = true;
    app.field = FIELD_NONE;

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(1100, 720, "Minecraft Builder");
    if (!IsWindowReady()) {
        mb_arena_destroy(&app.render);
        mb_arena_destroy(&app.plan);
        mb_arena_destroy(&app.model);
        return 1;
    }
    /* The settings card is laid out on a fixed vertical rhythm; 700 keeps the
     * save/test row inside the window even at the smallest allowed size. */
    SetWindowMinSize(1040, 700);
    SetTargetFPS(60);
    gui_style();
    for (int cp = 32; cp <= 126; cp++) app.fontCodepoints[app.fontCodepointCount++] = cp;
    font_add_visible_text(&app);
    for (int i = 0; i < MB_MATERIAL_COUNT; i++) font_add_text(&app, MB_MATERIALS[i].name);
    for (int i = 0; i < MB_STRUCTURE_COUNT; i++) {
        font_add_text(&app, MB_STRUCTURES[i].label);
        font_add_text(&app, MB_STRUCTURES[i].hint);
    }
    for (int i = 0; i < MB_STYLE_COUNT; i++) font_add_text(&app, MB_STYLES[i].label);
    font_reload(&app);

    /* Do not generate on launch: a model call costs tokens and a first-run user
     * may not have configured the endpoint yet. The empty viewport tells them
     * what to do instead. */
    set_status(&app, "填写描述后点击生成建筑");
    while (!WindowShouldClose()) {
        if (app.fontDirty) font_reload(&app);
        int width = GetScreenWidth();
        int height = GetScreenHeight();
        if (!app.inputReady) GuiLock();
        add_key_input(&app);

        BeginDrawing();
        app.inFrame = true;
        ClearBackground(UI_BG);
        draw_topbar(&app, width);
        draw_sidebar(&app, height);

        bool hasRightPanel = app.tab == GUI_VIEW_3D || app.tab == GUI_VIEW_PLAN;
        Rectangle central = central_bounds(width, height, hasRightPanel);
        float tabY = 102;
        float tabWidth = hasRightPanel ? (float)width - 648.0f : (float)width - 352.0f;
        for (int i = 0; i < 5; i++) {
            Rectangle tab = { 332.0f + i * tabWidth / 5.0f, tabY,
                              tabWidth / 5.0f - 4, 36 };
            if (i == app.tab) {
                DrawRectangleRounded(tab, 0.18f, 8, UI_GREEN);
                Vector2 textSize = MeasureTextEx(GuiGetFont(), GUI_TABS[i], 14, 1.0f);
                draw_text(GUI_TABS[i], tab.x + (tab.width - textSize.x) * 0.5f,
                          tab.y + 9, 14, UI_BG);
                if (app.inputReady && CheckCollisionPointRec(GetMousePosition(), tab) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    app.tab = i;
                    app.field = FIELD_NONE;
                }
            } else if (GuiButton(tab, GUI_TABS[i])) {
                app.tab = i;
                app.field = FIELD_NONE;
            }
        }

        if (app.tab == GUI_VIEW_3D) draw_preview(&app, central);
        else if (app.tab == GUI_VIEW_PLAN) draw_plan(&app, central);
        else if (app.tab == GUI_VIEW_REPORT) draw_report(&app, central);
        else if (app.tab == GUI_VIEW_EXPORT) draw_export(&app, central);
        else draw_settings(&app, central);
        draw_right_panel(&app, width, height);

        EndDrawing();
        app.inFrame = false;
        if (!app.inputReady) {
            GuiUnlock();
            app.inputReady = true;
        }
    }

    mb_config_save(&app.cfg);
    if (app.previewTexture.id) UnloadTexture(app.previewTexture);
    if (app.hasFont) UnloadFont(app.font);
    CloseWindow();
    mb_arena_destroy(&app.render);
    mb_arena_destroy(&app.plan);
    mb_arena_destroy(&app.model);
    return 0;
}
