/*
 * preview.c — the interactive 3D preview window.
 *
 * This is the only file in the project that owns a window. It is deliberately
 * thin: it keeps a camera, turns mouse and keyboard input into camera changes,
 * hands the camera to gfx3d.c, and then draws a bit of chrome on top of the
 * rendered pixels. All the actual rendering lives in gfx3d.c and stays portable.
 *
 * The frame buffer is a top-down 32bpp DIB section, and MBFramebuffer.color
 * points straight at its bits. There is no copy between rendering and display:
 * the rasteriser writes into the pixels the GPU is about to scan out.
 */
#include "mb.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#ifdef _WIN32

#include <windows.h>
#include <windowsx.h>   /* GET_X_LPARAM / GET_Y_LPARAM live here, not in winuser.h */

/* ========================================================================== */
/* state                                                                      */
/* ========================================================================== */

typedef struct {
    const MBBuildResult* result;

    HWND   hwnd;
    HDC    memdc;
    HBITMAP dib;
    HGDIOBJ dibPrev;
    void*  bits;
    int    w, h;

    Arena  frame;      /* scene scratch, reset every frame */
    Arena  text;       /* UTF-8 -> UTF-16 scratch, reset every text pass */

    double pivotX, pivotY, pivotZ;
    double yaw, pitch, dist, fov;

    MBSceneOptions opt;

    int    drag;       /* 0 none, 1 orbit, 2 pan */
    POINT  last;
    bool   quit;
} Preview;

static Preview g_pv;

/* ========================================================================== */
/* camera                                                                     */
/* ========================================================================== */

static void pv_camera(const Preview* p, MBCamera* cam)
{
    double right[3], up[3], fwd[3];
    memset(cam, 0, sizeof(*cam));
    cam->yaw   = p->yaw;
    cam->pitch = p->pitch;
    cam->fov   = p->fov;

    mb_camera_basis(cam, right, up, fwd);
    /* The camera sits `dist` behind the pivot, which is what keeps orbiting
       feeling like the model is being turned rather than the viewer flying. */
    cam->x = p->pivotX - fwd[0] * p->dist;
    cam->y = p->pivotY - fwd[1] * p->dist;
    cam->z = p->pivotZ - fwd[2] * p->dist;
}

static void pv_reset(Preview* p)
{
    const MBBuildResult* r = p->result;
    double sx = (double)r->size.x, sy = (double)r->size.y, sz = (double)r->size.z;

    p->pivotX = sx * 0.5;
    p->pivotY = sy * 0.45;
    p->pivotZ = sz * 0.5;

    p->yaw   = 0.62;
    p->pitch = -0.42;
    p->fov   = 0.90;

    double radius = 0.5 * sqrt(sx * sx + sy * sy + sz * sz);
    if (radius < 3.0) radius = 3.0;
    p->dist = radius / sin(p->fov * 0.5) * 1.18;

    mb_scene_default(&p->opt);
    p->drag = 0;
}

/* ========================================================================== */
/* UI painted into the pixel buffer                                           */
/* ========================================================================== */

static void blend_rect(MBFramebuffer* fb, int x0, int y0, int w, int h,
                       uint32_t col, double alpha)
{
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > fb->width)  w = fb->width  - x0;
    if (y0 + h > fb->height) h = fb->height - y0;
    if (w <= 0 || h <= 0) return;

    unsigned srcA = (unsigned)(alpha * 255.0 + 0.5);
    if (srcA > 255) srcA = 255;
    unsigned inv = 255u - srcA;

    unsigned sr = (col >> 16) & 0xFFu, sg = (col >> 8) & 0xFFu, sb = col & 0xFFu;

    for (int y = y0; y < y0 + h; y++) {
        uint32_t* row = fb->color + (size_t)y * (size_t)fb->width + (size_t)x0;
        /* opaque panels are worth special casing: it is by far the common case */
        if (srcA == 255) {
            for (int x = 0; x < w; x++) row[x] = col;
            continue;
        }
        for (int x = 0; x < w; x++) {
            uint32_t d = row[x];
            row[x] = ((unsigned)(((d >> 16) & 0xFFu) * inv + sr * srcA) / 255u << 16)
                   | ((unsigned)(((d >> 8)  & 0xFFu) * inv + sg * srcA) / 255u << 8)
                   |  (unsigned)(( d        & 0xFFu) * inv + sb * srcA) / 255u;
        }
    }
}

/* A rounded rectangle, approximated by one horizontal span per row. At a 12px
   radius the corners read as smooth without any real anti-aliasing work. */
static void fill_round(MBFramebuffer* fb, int x, int y, int w, int h,
                       int r, uint32_t col, double alpha)
{
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r <= 0) { blend_rect(fb, x, y, w, h, col, alpha); return; }

    for (int i = 0; i < h; i++) {
        double dy = 0.0;
        if (i < r)           dy = (double)(r - i) - 0.5;
        else if (i >= h - r) dy = (double)(i - (h - r)) + 0.5;

        int inset = 0;
        if (dy > 0.0) {
            double k = (double)r * (double)r - dy * dy;
            inset = r - (int)(sqrt(k > 0.0 ? k : 0.0) + 0.5);
        }
        blend_rect(fb, x + inset, y + i, w - 2 * inset, 1, col, alpha);
    }
}

/* A card: a 1px border drawn as the outer rounded rect, then the fill inset by
   one pixel so the border survives the corners. */
static void card_fill(MBFramebuffer* fb, int x, int y, int w, int h, int r,
                      uint32_t fill, double alpha, uint32_t border, double balpha)
{
    fill_round(fb, x, y, w, h, r, border, balpha);
    fill_round(fb, x + 1, y + 1, w - 2, h - 2, r - 1, fill, alpha);
}

static const char* const PANEL_KEYS[][2] = {
    { "移动",     "W A S D" },
    { "旋转视角", "Q / E"   },
    { "上升",     "Space"   },
    { "下降",     "Shift"   },
    { "环绕",     "拖动左键" },
    { "平移",     "拖动右键" },
    { "缩放",     "滚轮"    },
    { "切换网格", "G"       },
    { "切换坐标轴", "X"     },
    { "自动旋转", "O"       },
    { "重置视角", "R"       },
    { "退出",     "Esc"     },
};

#define PANEL_ROWS ((int)(sizeof PANEL_KEYS / sizeof PANEL_KEYS[0]))

#define TOPBAR_H   40
#define PANEL_W    248
#define PANEL_PAD  14
#define LINE_H     22

/* Right-hand "scene info" card rhythm. The four live state rows come first,
 * then the material distribution heading and its bars; the two functions that
 * paint that card (chrome and text) must agree on these or the bars land on top
 * of the labels. */
#define STATUS_STATS_TOP  62
#define STATUS_STATS_STEP 22
#define STATUS_CHART_TOP  180
#define STATUS_CHART_STEP 28
#define STATUS_BAR_OFFSET 17   /* thin bar sits under the material name */

/* ========================================================================== */
/* text                                                                       */
/* ========================================================================== */

static void text_draw(HDC hdc, Arena* a, HFONT font, int x, int y,
                      const char* utf8, COLORREF col)
{
    size_t n = 0;
    U16* w = mb_u16_from_utf8(a, utf8, &n);
    if (!w || n == 0) return;

    SelectObject(hdc, font);
    SetTextColor(hdc, col);
    SetBkMode(hdc, TRANSPARENT);
    TextOutW(hdc, x, y, (const WCHAR*)w, (int)n);
}

/* Right aligned variant: DrawTextW is the only sane way to do this without
   measuring the string by hand. */
static void text_draw_right(HDC hdc, Arena* a, HFONT font, int x, int y,
                            int w, int h, const char* utf8, COLORREF col)
{
    size_t n = 0;
    U16* s = mb_u16_from_utf8(a, utf8, &n);
    if (!s || n == 0) return;

    RECT rc;
    rc.left = x; rc.top = y; rc.right = x + w; rc.bottom = y + h;

    SelectObject(hdc, font);
    SetTextColor(hdc, col);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextW(hdc, (const WCHAR*)s, (int)n, &rc,
              DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}

static int text_width(HDC hdc, Arena* a, HFONT font, const char* utf8)
{
    size_t n = 0;
    U16* s = mb_u16_from_utf8(a, utf8, &n);
    if (!s || n == 0) return 0;

    SelectObject(hdc, font);
    SIZE size;
    if (!GetTextExtentPoint32W(hdc, (const WCHAR*)s, (int)n, &size)) return 0;
    return size.cx;
}

static HFONT make_font(int px, int weight)
{
    return CreateFontA(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                       "Microsoft YaHei");
}

/* ========================================================================== */
/* the frame                                                                  */
/* ========================================================================== */

static void pv_draw_chrome(Preview* p, MBFramebuffer* fb)
{
    /* The same tokens the terminal UI uses, so the two front ends feel like one
       product. Values are 0xRRGGBB. */
    const uint32_t canvas  = 0x16181D;   /* window backdrop                     */
    const uint32_t card    = 0x1E2128;   /* panel surface                       */
    const uint32_t raised  = 0x262A33;   /* chips and the active row            */
    const uint32_t line    = 0x333845;   /* hairline borders                    */
    const uint32_t accent  = 0x4CC38A;   /* primary accent — header, active     */
    const uint32_t accent2 = 0x8B7BF0;   /* secondary accent — badge            */
    const uint32_t gold    = 0xE0B15E;   /* warm tertiary — headings            */
    const uint32_t ink     = 0xE7EAF0;
    const uint32_t soft    = 0xC3C9D6;
    const uint32_t dim     = 0x8B93A7;

    (void)ink; (void)soft; (void)dim; (void)gold;   /* used by pv_draw_text() */

    int w = fb->width;
    int h = fb->height;

    /* Legibility wash over the rendered scene, then a vignette-free flat base. */
    blend_rect(fb, 0, 0, w, h, canvas, 0.30);

    /* --- top bar ---------------------------------------------------------- */
    int barH = 46;
    blend_rect(fb, 0, 0, w, barH, card, 0.96);
    blend_rect(fb, 0, barH - 1, w, 1, line, 0.9);
    /* accent hairline along the very top edge — the one loud detail up here */
    blend_rect(fb, 0, 0, w, 2, accent, 0.85);

    /* Brand mark: a filled accent square with a lighter core. */
    fill_round(fb, 16, 14, 20, 20, 6, accent, 0.95);
    fill_round(fb, 21, 19, 10, 10, 3, card, 1.0);

    /* Engine chip on the right of the bar. */
    {
        const char* label = (p->result && p->result->report.isLlm) ? "AI 规划" : "本地规划";
        int chipW = (label[0] == 'A') ? 74 : 88;
        int cx = w - chipW - 16;
        fill_round(fb, cx, 13, chipW, 22, 11,
                   (p->result && p->result->report.isLlm) ? accent2 : raised, 0.95);
    }

    /* --- left panel: controls -------------------------------------------- */
    int pad = PANEL_PAD;
    int leftW = PANEL_W;
    int leftX = pad;
    int leftY = barH + pad;
    int leftH = h - leftY - 64;
    if (leftH < 120) leftH = 120;
    card_fill(fb, leftX, leftY, leftW, leftH, 12, card, 0.95, line, 0.85);

    /* Row backgrounds for the keybinding list, plus the material swatch strip. */
    int rowY = leftY + 92;
    for (int i = 0; i < PANEL_ROWS; i++) {
        int y = rowY + i * LINE_H;
        if (y + LINE_H > leftY + leftH - 12) break;
        if ((i & 1) == 0) {
            fill_round(fb, leftX + 8, y - 2, leftW - 16, LINE_H - 2, 6, raised, 0.45);
        }
    }

    /* --- right panel: scene status --------------------------------------- */
    int rightW = PANEL_W;
    int rightX = w - rightW - pad;
    int rightY = barH + pad;
    int rightH = h - rightY - 64;
    if (rightH < 120) rightH = 120;
    if (rightX > leftX + leftW + 16) {
        card_fill(fb, rightX, rightY, rightW, rightH, 12, card, 0.95, line, 0.85);

        /* A small proportional bar chart of the build's material mix. */
        const MBBuildResult* r = p->result;
        if (r && r->byMaterialCount > 0 && r->total > 0) {
            int chartX = rightX + 14;
            int chartW = rightW - 28;
            int cy = rightY + STATUS_CHART_TOP;
            int maxRows = 6;
            for (int i = 0; i < r->byMaterialCount && i < maxRows; i++) {
                double frac = (double)r->byMaterial[i].count / (double)r->total;
                int barW = (int)(frac * (double)chartW + 0.5);
                if (barW < 3) barW = 3;
                uint32_t col = mb_material_color(r->byMaterial[i].id);
                int y = cy + i * STATUS_CHART_STEP;
                if (y + STATUS_CHART_STEP > rightY + rightH - 12) break;
                blend_rect(fb, chartX, y + STATUS_BAR_OFFSET, chartW, 4, line, 0.55);
                fill_round(fb, chartX, y + STATUS_BAR_OFFSET, barW, 4, 2, col, 0.95);
            }
        }
    }

    /* --- bottom status bar ------------------------------------------------ */
    int sh = 40;
    blend_rect(fb, 0, h - sh, w, sh, card, 0.96);
    blend_rect(fb, 0, h - sh, w, 1, line, 0.9);
    /* soft colour chips showing what this build is made of */
    {
        const MBBuildResult* r = p->result;
        int x = 16;
        for (int i = 0; r && i < r->byMaterialCount && i < 7; i++) {
            uint32_t col = mb_material_color(r->byMaterial[i].id);
            fill_round(fb, x, h - sh + 14, 12, 12, 4, col, 0.95);
            x += 16;
        }
        if (r && r->byMaterialCount > 7) {
            int moreX = x;
            (void)moreX;   /* the "+N" text is drawn by pv_draw_text() */
        }
    }

    /* a compact grid/axes toggle indicator on the far right of the status bar */
    {
        int bx = w - 16 - 12 - 12 - 8;
        fill_round(fb, bx, h - sh + 14, 12, 12, 3,
                   p->opt.showGrid ? accent : line, 0.9);
        fill_round(fb, bx + 20, h - sh + 14, 12, 12, 3,
                   p->opt.showAxes ? accent2 : line, 0.9);
    }
}

static void pv_draw_text(Preview* p, HDC hdc)
{
    const MBBuildResult* r = p->result;
    const char* name = (r && r->name && *r->name) ? r->name : "Minecraft Builder";
    Arena* a = &p->text;
    mb_arena_reset(a);

    HFONT fTitle  = make_font(17, FW_SEMIBOLD);
    HFONT fSub    = make_font(12, FW_NORMAL);
    HFONT fHead   = make_font(13, FW_SEMIBOLD);
    HFONT fBody   = make_font(13, FW_NORMAL);
    HFONT fSmall  = make_font(12, FW_NORMAL);
    HFONT fMono   = make_font(12, FW_NORMAL);

    const COLORREF ink    = RGB(0xE7, 0xEA, 0xF0);
    const COLORREF soft   = RGB(0xC3, 0xC9, 0xD6);
    const COLORREF dim    = RGB(0x8B, 0x93, 0xA7);
    const COLORREF accent = RGB(0x4C, 0xC3, 0x8A);
    const COLORREF accent2= RGB(0x8B, 0x7B, 0xF0);
    const COLORREF gold   = RGB(0xE0, 0xB1, 0x5E);

    int w = p->w, h = p->h;
    const int barH = 46;
    const int pad  = PANEL_PAD;

    /* --- top bar ---------------------------------------------------------- */
    text_draw(hdc, a, fTitle, 46, 12, name, ink);
    {
        char sub[160];
        if (r) {
            snprintf(sub, sizeof sub, "%d×%d×%d · %d 方块 · %d 种材质",
                     r->size.x, r->size.y, r->size.z, r->total, r->byMaterialCount);
        } else {
            snprintf(sub, sizeof sub, "3D 预览");
        }
        text_draw(hdc, a, fSub, 46, 31, sub, dim);
        /* The chip background was painted in pv_draw_chrome(). */
        const char* chip = (r && r->report.isLlm) ? "AI 规划" : "本地规划";
        int chipW = (chip[0] == 'A') ? 74 : 88;
        text_draw(hdc, a, fSmall, w - chipW - 16 + 14, 17, chip,
                  (r && r->report.isLlm) ? RGB(0x14, 0x12, 0x24) : soft);
    }

    /* --- left card: keybinds --------------------------------------------- */
    int leftX = pad, leftY = barH + pad, leftW = PANEL_W;
    text_draw(hdc, a, fHead, leftX + 16, leftY + 14, "操作指引", gold);
    text_draw(hdc, a, fSmall, leftX + 16, leftY + 36, "键鼠控制", dim);

    int rowY = leftY + 92;
    for (int i = 0; i < PANEL_ROWS; i++) {
        int y = rowY + i * LINE_H;
        if (y + LINE_H > leftY + (h - leftY - 64) - 12) break;
        text_draw(hdc, a, fBody, leftX + 16, y + 1, PANEL_KEYS[i][0], soft);
        text_draw_right(hdc, a, fMono, leftX + 16, y, leftW - 32, LINE_H,
                        PANEL_KEYS[i][1], gold);
    }

    /* --- right card: scene status ---------------------------------------- */
    int rightW = PANEL_W, rightX = w - rightW - pad, rightY = barH + pad;
    if (rightX > leftX + leftW + 16) {
        text_draw(hdc, a, fHead, rightX + 16, rightY + 14, "场景信息", gold);
        text_draw(hdc, a, fSmall, rightX + 16, rightY + 36, "实时状态", dim);

        char buf[160];
        int ly = rightY + STATUS_STATS_TOP;
        const int step = STATUS_STATS_STEP;

        snprintf(buf, sizeof buf, "%.1f", p->dist);
        text_draw(hdc, a, fSmall, rightX + 16, ly, "相机距离", dim);
        text_draw_right(hdc, a, fMono, rightX + 16, ly, rightW - 32, 16, buf, soft);
        ly += step;

        text_draw(hdc, a, fSmall, rightX + 16, ly, "网格", dim);
        text_draw_right(hdc, a, fMono, rightX + 16, ly, rightW - 32, 16,
                        p->opt.showGrid ? "开" : "关",
                        p->opt.showGrid ? accent : dim);
        ly += step;

        text_draw(hdc, a, fSmall, rightX + 16, ly, "坐标轴", dim);
        text_draw_right(hdc, a, fMono, rightX + 16, ly, rightW - 32, 16,
                        p->opt.showAxes ? "开" : "关",
                        p->opt.showAxes ? accent2 : dim);
        ly += step;

        text_draw(hdc, a, fSmall, rightX + 16, ly, "自动旋转", dim);
        text_draw_right(hdc, a, fMono, rightX + 16, ly, rightW - 32, 16,
                        p->opt.autoOrbit ? "开" : "关",
                        p->opt.autoOrbit ? accent : dim);

        /* material distribution heading sits just above the bars */
        text_draw(hdc, a, fSmall, rightX + 16, rightY + STATUS_CHART_TOP - 22,
                  "材质占比", dim);

        if (r && r->byMaterialCount > 0 && r->total > 0) {
            int cy = rightY + STATUS_CHART_TOP;
            for (int i = 0; i < r->byMaterialCount && i < 6; i++) {
                int y = cy + i * STATUS_CHART_STEP;
                if (y + STATUS_CHART_STEP > rightY + (h - rightY - 64) - 12) break;
                const char* nm = mb_material_name(r->byMaterial[i].id);
                double pct = 100.0 * (double)r->byMaterial[i].count / (double)r->total;
                char val[48];
                snprintf(val, sizeof val, "%.0f%%", pct);
                text_draw(hdc, a, fSmall, rightX + 16, y, nm, soft);
                text_draw_right(hdc, a, fMono, rightX + 16, y, rightW - 32, 16, val, dim);
            }
        } else {
            text_draw(hdc, a, fSmall, rightX + 16, rightY + STATUS_CHART_TOP,
                      "暂无数据", dim);
        }
    }

    /* --- bottom status bar ------------------------------------------------ */
    int sh = 40;
    int by = h - sh + 11;

    /* Leave the material swatch strip its own space; text starts after it. */
    int swatches = 0;
    if (r) swatches = r->byMaterialCount > 7 ? 7 : r->byMaterialCount;
    int coordX = 16 + swatches * 16 + (swatches ? 4 : 0);

    char coord[128];
    snprintf(coord, sizeof coord, "视角 X %.0f  Y %.0f  Z %.0f",
             p->pivotX, p->pivotY, p->pivotZ);
    text_draw(hdc, a, fSmall, coordX, by, coord, soft);

    if (r && r->byMaterialCount > 7) {
        char more[32];
        snprintf(more, sizeof more, "+%d", r->byMaterialCount - 7);
        int coordW = text_width(hdc, a, fSmall, coord);
        text_draw(hdc, a, fSmall, coordX + coordW + 8, by, more, dim);
    }

    /* toggles legend on the far right, next to the two chips */
    {
        int bx = w - 16 - 12 - 12 - 8;
        text_draw_right(hdc, a, fSmall, bx - 190, h - sh + 9, 180, 22,
                        "网格   坐标轴", dim);
    }

    DeleteObject(fTitle);
    DeleteObject(fSub);
    DeleteObject(fHead);
    DeleteObject(fBody);
    DeleteObject(fSmall);
    DeleteObject(fMono);
}

/* ========================================================================== */
/* DIB plumbing                                                               */
/* ========================================================================== */

static void pv_free_dib(Preview* p)
{
    if (p->dib && p->memdc) {
        SelectObject(p->memdc, p->dibPrev);
        DeleteObject(p->dib);
    }
    p->dib = NULL;
    p->bits = NULL;
}

static bool pv_alloc_dib(Preview* p, int w, int h)
{
    if (w < 64) w = 64;
    if (h < 64) h = 64;
    if (w == p->w && h == p->h && p->dib) return true;

    pv_free_dib(p);

    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;    /* negative height: row 0 is the top row */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = NULL;
    p->dib = CreateDIBSection(p->memdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!p->dib || !bits) return false;

    p->dibPrev = SelectObject(p->memdc, p->dib);
    p->bits = bits;
    p->w = w;
    p->h = h;
    return true;
}

static void pv_paint(Preview* p, HDC hdc)
{
    if (!p->dib) return;

    mb_arena_reset(&p->frame);

    MBFramebuffer fb;
    fb.width  = p->w;
    fb.height = p->h;
    fb.color  = (uint32_t*)p->bits;
    fb.depth  = (float*)mb_arena_alloc(&p->frame,
                                       (size_t)p->w * (size_t)p->h * sizeof(float));
    if (!fb.depth) return;

    MBCamera cam;
    pv_camera(p, &cam);
    mb_render_scene(&p->frame, &fb, p->result, &cam, &p->opt);

    pv_draw_chrome(p, &fb);
    BitBlt(hdc, 0, 0, p->w, p->h, p->memdc, 0, 0, SRCCOPY);
    pv_draw_text(p, hdc);
}

/* ========================================================================== */
/* input                                                                      */
/* ========================================================================== */

static bool key_down(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static void pv_move_pivot(Preview* p, double dx, double dy, double dz)
{
    p->pivotX += dx;
    p->pivotY += dy;
    p->pivotZ += dz;
}

static void pv_tick(Preview* p)
{
    if (!p->hwnd) return;

    MBCamera cam;
    pv_camera(p, &cam);

    double right[3], up[3], fwd[3];
    mb_camera_basis(&cam, right, up, fwd);

    /* W/S travel along the ground rather than along the view vector: flying
       forward into the dirt when you meant to walk is the classic free camera
       annoyance. Space/Shift are there for altitude. */
    double hf[3] = { fwd[0], 0.0, fwd[2] };
    double hl = sqrt(hf[0] * hf[0] + hf[2] * hf[2]);
    if (hl > 1e-6) { hf[0] /= hl; hf[2] /= hl; }

    double step = p->dist * 0.012 + 0.12;
    double dx = 0, dy = 0, dz = 0;
    bool moved = false;

    if (key_down('W')) { dx += hf[0] * step; dz += hf[2] * step; moved = true; }
    if (key_down('S')) { dx -= hf[0] * step; dz -= hf[2] * step; moved = true; }
    if (key_down('D')) { dx += right[0] * step; dz += right[2] * step; moved = true; }
    if (key_down('A')) { dx -= right[0] * step; dz -= right[2] * step; moved = true; }
    if (key_down(VK_SPACE)) { dy += step; moved = true; }
    if (key_down(VK_SHIFT)) { dy -= step; moved = true; }

    if (key_down('Q')) { p->yaw -= 0.022; moved = true; }
    if (key_down('E')) { p->yaw += 0.022; moved = true; }
    if (p->opt.autoOrbit) { p->yaw += 0.004; moved = true; }

    if (moved) {
        pv_move_pivot(p, dx, dy, dz);
        InvalidateRect(p->hwnd, NULL, FALSE);
    }
}

static void pv_zoom(Preview* p, double factor)
{
    p->dist *= factor;
    if (p->dist < 1.0)  p->dist = 1.0;
    if (p->dist > 400.0) p->dist = 400.0;
}

static LRESULT CALLBACK pv_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Preview* p = &g_pv;

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;    /* the frame covers every pixel; skipping this stops flicker */

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        pv_paint(p, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SIZE:
        if (pv_alloc_dib(p, LOWORD(lp), HIWORD(lp))) {
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_TIMER:
        pv_tick(p);
        return 0;

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        pv_zoom(p, delta > 0 ? 0.88 : 1.0 / 0.88);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
        p->drag = 1;
        p->last.x = GET_X_LPARAM(lp);
        p->last.y = GET_Y_LPARAM(lp);
        SetCapture(hwnd);
        return 0;

    case WM_RBUTTONDOWN:
        p->drag = 2;
        p->last.x = GET_X_LPARAM(lp);
        p->last.y = GET_Y_LPARAM(lp);
        SetCapture(hwnd);
        return 0;

    case WM_MOUSEMOVE: {
        if (p->drag == 0) return 0;
        int x = GET_X_LPARAM(lp);
        int y = GET_Y_LPARAM(lp);
        int mx = x - p->last.x;
        int my = y - p->last.y;
        p->last.x = x;
        p->last.y = y;

        if (p->drag == 1) {
            /* orbit: a full drag across the window is about 180 degrees */
            p->yaw   += mx * 0.0075;
            p->pitch -= my * 0.0060;
            if (p->pitch >  1.45) p->pitch =  1.45;
            if (p->pitch < -1.45) p->pitch = -1.45;
        } else {
            MBCamera cam;
            pv_camera(p, &cam);
            double right[3], up[3], fwd[3];
            mb_camera_basis(&cam, right, up, fwd);
            /* panning scales with distance so it feels the same zoomed in or out */
            double k = p->dist * 0.0016;
            pv_move_pivot(p, (-right[0] * mx + up[0] * my) * k,
                             (-right[1] * mx + up[1] * my) * k,
                             (-right[2] * mx + up[2] * my) * k);
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP:
        if (p->drag) {
            p->drag = 0;
            ReleaseCapture();
        }
        return 0;

    case WM_KEYDOWN:
        switch (wp) {
        case VK_ESCAPE: DestroyWindow(hwnd); return 0;
        case 'R': pv_reset(p); InvalidateRect(hwnd, NULL, FALSE); return 0;
        case 'G':
            p->opt.showGrid = !p->opt.showGrid;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case 'X':
            p->opt.showAxes = !p->opt.showAxes;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case 'O':
            p->opt.autoOrbit = !p->opt.autoOrbit;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        default:
            return 0;
        }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        p->quit = true;
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

/* ========================================================================== */
/* entry point                                                                */
/* ========================================================================== */

/* Opt into per-monitor DPI scaling, but only if the OS has the call: the
   GetProcAddress dance avoids a hard dependency on a Vista-era user32 export. */
static void enable_dpi_awareness(void)
{
    typedef BOOL (WINAPI *SetDpiAwareFn)(void);
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (!user32) return;

    /* Two-step through void(*)(void): that is the one intermediate conversion
       -Wcast-function-type is happy to let past. */
    void (*raw)(void) = (void (*)(void))GetProcAddress(user32, "SetProcessDPIAware");
    if (!raw) return;
    SetDpiAwareFn fn = (SetDpiAwareFn)raw;
    fn();
}

bool mb_preview_open(const MBBuildResult* r, const char* title, bool hideConsole)
{
    if (!r) return false;

    memset(&g_pv, 0, sizeof g_pv);
    g_pv.result = r;
    g_pv.memdc  = CreateCompatibleDC(NULL);
    if (!g_pv.memdc) return false;

    mb_arena_init(&g_pv.frame);
    mb_arena_init(&g_pv.text);
    pv_reset(&g_pv);

    enable_dpi_awareness();

    HINSTANCE inst = GetModuleHandleA(NULL);

    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = pv_proc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = "MBPreviewWnd";
    RegisterClassA(&wc);   /* harmless if it is already registered */

    /* UTF-8 title in, UTF-16 title out — same reasoning as the overlay text. */
    size_t cap = 0;
    U16* wtitle = mb_u16_from_utf8(&g_pv.text, title && *title ? title : "3D 预览", &cap);
    if (!wtitle) { DeleteDC(g_pv.memdc); return false; }

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int cw = sw * 2 / 3, ch = sh * 2 / 3;
    if (cw < 800)  cw = 800;
    if (ch < 560)  ch = 560;

    HWND hwnd = CreateWindowExA(
        0, "MBPreviewWnd", "3D 预览",
        WS_OVERLAPPEDWINDOW,
        (sw - cw) / 2, (sh - ch) / 2, cw, ch,
        NULL, NULL, inst, NULL);

    if (!hwnd) {
        MessageBoxA(NULL, "创建预览窗口失败。", "3D 预览", MB_ICONERROR | MB_OK);
        DeleteDC(g_pv.memdc);
        mb_arena_destroy(&g_pv.frame);
        mb_arena_destroy(&g_pv.text);
        return false;
    }

    SetWindowTextW(hwnd, (const WCHAR*)wtitle);
    g_pv.hwnd = hwnd;

    HWND console = NULL;
    if (hideConsole) {
        console = GetConsoleWindow();
        if (console) ShowWindow(console, SW_HIDE);
    }

    RECT rc;
    GetClientRect(hwnd, &rc);
    pv_alloc_dib(&g_pv, rc.right - rc.left, rc.bottom - rc.top);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetTimer(hwnd, 1, 16, NULL);

    MSG msg;
    while (!g_pv.quit && GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    if (console) {
        ShowWindow(console, SW_SHOW);
        SetForegroundWindow(console);
    }

    pv_free_dib(&g_pv);
    DeleteDC(g_pv.memdc);
    mb_arena_destroy(&g_pv.frame);
    mb_arena_destroy(&g_pv.text);
    g_pv.hwnd = NULL;
    return true;
}

#else  /* !_WIN32 */

/*
 * The windowed front end is Windows only. Everything else — the geometry, the
 * model calls, the terminal UI, the offscreen renderer — is portable, so this
 * stub is all a non-Windows build needs.
 */
bool mb_preview_open(const MBBuildResult* r, const char* title, bool hideConsole)
{
    (void)r;
    (void)title;
    (void)hideConsole;
    return false;
}

#endif /* _WIN32 */
