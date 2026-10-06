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

static const char* const PANEL_KEYS[][2] = {
    { "移动",     "W A S D" },
    { "旋转视角", "Q / E"   },
    { "上升",     "Space"   },
    { "下降",     "Shift"   },
    { "环绕",     "拖动左键" },
    { "平移",     "拖动右键" },
    { "缩放",     "滚轮"    },
    { "切换网格", "G"       },
    { "重置视角", "R"       },
    { "退出",     "Esc"     },
};

#define PANEL_ROWS ((int)(sizeof PANEL_KEYS / sizeof PANEL_KEYS[0]))

#define TOPBAR_H   34
#define PANEL_W    232
#define PANEL_PAD  12
#define LINE_H     22

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
    const uint32_t panel = 0x0F1114;
    const uint32_t edge  = 0x3A4048;

    /* --- top bar ---------------------------------------------------------- */
    blend_rect(fb, 0, 0, fb->width, TOPBAR_H, panel, 0.78);
    blend_rect(fb, 0, TOPBAR_H - 1, fb->width, 1, edge, 0.55);

    /* --- key reference panel, right hand side ----------------------------- */
    int ph = PANEL_PAD * 2 + LINE_H * (PANEL_ROWS + 1) + 6;
    int py = TOPBAR_H + 14;
    int px = fb->width - PANEL_W - 14;
    if (px > 16 && py + ph < fb->height - 16) {
        blend_rect(fb, px, py, PANEL_W, ph, panel, 0.72);
        blend_rect(fb, px, py, PANEL_W, 1, edge, 0.75);
        blend_rect(fb, px, py + ph - 1, PANEL_W, 1, edge, 0.5);
    }

    /* --- camera readout, bottom left -------------------------------------- */
    char buf[96];
    snprintf(buf, sizeof buf, "X %d  Y %d  Z %d",
             (int)floor(p->pivotX + 0.5),
             (int)floor(p->pivotY + 0.5),
             (int)floor(p->pivotZ + 0.5));
    int bw = 8 + (int)strlen(buf) * 9;
    blend_rect(fb, 14, fb->height - 46, bw, 30, panel, 0.70);
    blend_rect(fb, 14, fb->height - 46, bw, 1, edge, 0.55);
}

static void pv_draw_text(Preview* p, HDC hdc)
{
    const MBBuildResult* r = p->result;
    Arena* a = &p->text;
    mb_arena_reset(a);

    HFONT fTitle = make_font(16, FW_SEMIBOLD);
    HFONT fBody  = make_font(13, FW_NORMAL);
    HFONT fSmall = make_font(12, FW_NORMAL);

    const COLORREF ink   = RGB(0xEC, 0xEF, 0xF2);
    const COLORREF dim   = RGB(0xA8, 0xB0, 0xB8);
    const COLORREF faint = RGB(0x7C, 0x86, 0x90);
    const COLORREF key   = RGB(0x9E, 0xD6, 0x7E);

    /* --- title bar -------------------------------------------------------- */
    const char* name = (r->name && *r->name) ? r->name : "Minecraft Builder";
    text_draw(hdc, a, fTitle, 14, 8, name, ink);

    char info[192];
    snprintf(info, sizeof info, "%d x %d x %d   %d 方块   %d 种材质",
             r->size.x, r->size.y, r->size.z,
             (int)r->blockCount, (int)r->paletteCount);
    text_draw(hdc, a, fSmall, 14 + (int)strlen(name) * 9 + 16, 13, info, faint);

    /* --- key panel -------------------------------------------------------- */
    int py = TOPBAR_H + 14;
    int px = p->w - PANEL_W - 14;
    if (px > 16 && py + 400 < p->h) {
        text_draw(hdc, a, fTitle, px + PANEL_PAD, py + PANEL_PAD - 2, "操作说明", ink);
        for (int i = 0; i < PANEL_ROWS; i++) {
            int rowY = py + PANEL_PAD + LINE_H * (i + 1) + 4;
            text_draw(hdc, a, fBody, px + PANEL_PAD, rowY, PANEL_KEYS[i][0], dim);
            text_draw_right(hdc, a, fBody, px + PANEL_PAD, rowY,
                            PANEL_W - PANEL_PAD * 2, LINE_H, PANEL_KEYS[i][1], key);
        }
    }

    /* --- readout ---------------------------------------------------------- */
    char buf[96];
    snprintf(buf, sizeof buf, "X %d  Y %d  Z %d",
             (int)floor(p->pivotX + 0.5),
             (int)floor(p->pivotY + 0.5),
             (int)floor(p->pivotZ + 0.5));
    text_draw(hdc, a, fBody, 14 + 10, p->h - 46 + 7, buf, ink);

    char hint[128];
    snprintf(hint, sizeof hint, "距离 %.1f   视角 %d%%   网格 %s",
             p->dist, (int)(p->fov * 180.0 / 3.14159265358979),
             p->opt.showGrid ? "开" : "关");
    text_draw(hdc, a, fSmall, 14 + 10 + 150, p->h - 46 + 8, hint, faint);

    DeleteObject(fTitle);
    DeleteObject(fBody);
    DeleteObject(fSmall);
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
