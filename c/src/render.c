/*
 * render.c — the preview.
 *
 * Two views, both emitted as ANSI true colour escape sequences:
 *
 *   mb_render_iso()    an isometric 3D preview. The rasteriser works on a pixel
 *                      buffer twice as tall as the character grid and emits the
 *                      Unicode half block "▀" with a foreground colour for the
 *                      upper pixel and a background colour for the lower one, so
 *                      one character cell carries two square pixels. That gives
 *                      a genuinely blocky but correctly proportioned 3D view.
 *
 *   mb_render_slice()  a top down floor plan of a single Y layer, one character
 *                      per block, which is what you want when you are checking
 *                      the layout rather than the silhouette.
 *
 * Cubes are drawn back to front (painter's algorithm, sorted by distance along
 * the camera axis), and each cube is shaded by face: the top face is brightest,
 * then the two visible sides.
 */
#include "mb.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* small colour helpers                                                       */
/* ========================================================================== */

typedef struct { unsigned char r, g, b; } RGB;

/* Shared design tokens. The terminal views and the window chrome all read from
   this palette so the whole product looks like one thing. */
#define UI_CANVAS_R 22
#define UI_CANVAS_G 24
#define UI_CANVAS_B 29
#define UI_GRID_R   44
#define UI_GRID_G   49
#define UI_GRID_B   60
#define UI_DIM_R    139
#define UI_DIM_G    147
#define UI_DIM_B    167
#define UI_SOFT_R   195
#define UI_SOFT_G   201
#define UI_SOFT_B   214
#define UI_ACCENT_R 76
#define UI_ACCENT_G 195
#define UI_ACCENT_B 138
#define UI_VOID_R   24
#define UI_VOID_G   26
#define UI_VOID_B   33

static RGB rgb_of(uint32_t color)
{
    RGB c;
    c.r = (unsigned char)((color >> 16) & 0xFF);
    c.g = (unsigned char)((color >> 8) & 0xFF);
    c.b = (unsigned char)(color & 0xFF);
    return c;
}

static RGB shade(RGB base, double k)
{
    RGB c;
    int r = (int)(base.r * k + 0.5);
    int g = (int)(base.g * k + 0.5);
    int b = (int)(base.b * k + 0.5);
    c.r = (unsigned char)(r > 255 ? 255 : (r < 0 ? 0 : r));
    c.g = (unsigned char)(g > 255 ? 255 : (g < 0 ? 0 : g));
    c.b = (unsigned char)(b > 255 ? 255 : (b < 0 ? 0 : b));
    return c;
}

static bool rgb_eq(RGB a, RGB b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

static void emit_fg(SBuf* out, RGB c)
{
    mb_sb_printf(out, "\x1b[38;2;%d;%d;%dm", c.r, c.g, c.b);
}

static void emit_bg(SBuf* out, RGB c)
{
    mb_sb_printf(out, "\x1b[48;2;%d;%d;%dm", c.r, c.g, c.b);
}

/* ========================================================================== */
/* view                                                                       */
/* ========================================================================== */

void mb_view_default(MBView* v)
{
    v->yaw = 0.0;
    v->pitch = 0.5235987755982988;   /* pi / 6 -> classic 2:1 isometric */
    v->zoom = 0.0;                   /* 0 = fit to the viewport */
    v->showGrid = true;
    v->showAxes = true;
}

/* depthScale turns the pitch into a vertical stretch of the ground plane. */
static double depth_scale(double pitch)
{
    double s = sin(pitch);
    if (s < 0.15) s = 0.15;
    if (s > 0.95) s = 0.95;
    return s / 0.5;                  /* 1.0 at the default pi/6 */
}

/* ========================================================================== */
/* isometric renderer                                                         */
/* ========================================================================== */

typedef struct {
    double   cx, cz;        /* model centre in world x/z */
    double   cosY, sinY;    /* yaw */
    double   depthScale;
    double   zoom;
    double   offsetX, offsetY;
} Iso;

static void iso_project(const Iso* m, double x, double y, double z,
                        double* outX, double* outY)
{
    double rx = (x - m->cx) * m->cosY - (z - m->cz) * m->sinY;
    double rz = (x - m->cx) * m->sinY + (z - m->cz) * m->cosY;
    *outX = (rx - rz) * 0.8660254037844386 * m->zoom + m->offsetX;
    *outY = (rx + rz) * 0.5 * m->depthScale * m->zoom - y * m->zoom + m->offsetY;
}

static void put_pixel(RGB* px, int W, int H, int x, int y, RGB c)
{
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    px[(size_t)y * (size_t)W + (size_t)x] = c;
}

/*
 * Fill the silhouette hexagon of one cube.
 *
 * `cxp` / `cyp` are the centre of the *top face*; the shape is
 *
 *        (0,-v)
 *   (-w,0)     (w,0)
 *   (-w,H)     (w,H)
 *        (0,H+v)
 *
 * with H = cube height in pixels. Pixels inside the top face rhombus get the
 * brightest shade, the rest is split left/right into the two visible sides.
 */
static void draw_cube(RGB* px, int W, int H, double cxp, double cyp,
                      double w, double v, double cubeH, RGB base)
{
    RGB top = shade(base, 1.00);
    RGB leftFace = shade(base, 0.56);
    RGB rightFace = shade(base, 0.78);

    if (w < 0.5) {
        /* Too small to resolve a cube: paint a single pixel so the shape of the
           building is still visible. */
        put_pixel(px, W, H, (int)floor(cxp + 0.5), (int)floor(cyp + 0.5), top);
        return;
    }

    int y0 = (int)floor(cyp - v);
    int y1 = (int)ceil(cyp + cubeH + v);
    int x0 = (int)floor(cxp - w);
    int x1 = (int)ceil(cxp + w);

    for (int py = y0; py <= y1; py++) {
        double dy = (double)py - cyp;          /* relative to the top face centre */
        double halfW;
        if (dy < 0.0)       halfW = w * (dy + v) / v;
        else if (dy <= cubeH) halfW = w;
        else                halfW = w * (cubeH + v - dy) / v;
        if (halfW <= 0.0) continue;

        for (int pxx = x0; pxx <= x1; pxx++) {
            double dx = (double)pxx - cxp;
            double adx = dx < 0 ? -dx : dx;
            if (adx > halfW) continue;

            /* inside the top face rhombus? */
            double rhombus = adx / w + (dy < 0 ? -dy : dy) / v;
            RGB c;
            if (rhombus <= 1.0) c = top;
            else if (dx < 0.0)  c = leftFace;
            else                c = rightFace;

            put_pixel(px, W, H, pxx, py, c);
        }
    }
}

static void draw_line_3d(RGB* px, int W, int H, const Iso* m,
                         double x1, double y1, double z1,
                         double x2, double y2, double z2, RGB c,
                         int steps)
{
    for (int i = 0; i <= steps; i++) {
        double t = (double)i / (double)steps;
        double X, Y;
        iso_project(m, x1 + (x2 - x1) * t, y1 + (y2 - y1) * t, z1 + (z2 - z1) * t, &X, &Y);
        /* a 2x2 stamp keeps thin lines visible at small zoom values */
        put_pixel(px, W, H, (int)floor(X + 0.5), (int)floor(Y + 0.5), c);
        put_pixel(px, W, H, (int)floor(X + 0.5) + 1, (int)floor(Y + 0.5), c);
        put_pixel(px, W, H, (int)floor(X + 0.5), (int)floor(Y + 0.5) + 1, c);
        put_pixel(px, W, H, (int)floor(X + 0.5) + 1, (int)floor(Y + 0.5) + 1, c);
    }
}

char* mb_render_iso(Arena* a, const MBBuildResult* r, const MBView* v, int cols, int rows)
{
    const int W = cols;
    const int H = rows * 2;          /* two pixels per character cell */
    if (W < 8 || H < 8) return mb_arena_strdup(a, "");

    RGB* px = (RGB*)mb_arena_alloc(a, (size_t)W * (size_t)H * sizeof(RGB));
    RGB bg = { UI_CANVAS_R, UI_CANVAS_G, UI_CANVAS_B };
    for (size_t i = 0; i < (size_t)W * (size_t)H; i++) px[i] = bg;

    Iso m;
    memset(&m, 0, sizeof(m));
    m.cx = r->size.x / 2.0;
    m.cz = r->size.z / 2.0;
    m.cosY = cos(v->yaw);
    m.sinY = sin(v->yaw);
    m.depthScale = depth_scale(v->pitch);

    /* --- work out the zoom that makes the building fit ------------------- */
    {
        double hx = r->size.x / 2.0;
        double hz = r->size.z / 2.0;
        double corners[4][2] = { { -hx, -hz }, { hx, -hz }, { hx, hz }, { -hx, hz } };
        double xmin = 1e30, xmax = -1e30, ymin = 1e30, ymax = -1e30;

        for (int i = 0; i < 4; i++) {
            double rx = corners[i][0] * m.cosY - corners[i][1] * m.sinY;
            double rz = corners[i][0] * m.sinY + corners[i][1] * m.cosY;
            double X = (rx - rz) * 0.8660254037844386;
            double Ytop = (rx + rz) * 0.5 * m.depthScale - (double)r->size.y;
            double Ybot = (rx + rz) * 0.5 * m.depthScale;
            if (X < xmin) xmin = X;
            if (X > xmax) xmax = X;
            if (Ytop < ymin) ymin = Ytop;
            if (Ybot > ymax) ymax = Ybot;
        }

        double w1 = xmax - xmin;
        double h1 = ymax - ymin;
        if (w1 < 1e-6) w1 = 1e-6;
        if (h1 < 1e-6) h1 = 1e-6;

        m.zoom = v->zoom > 0.0 ? v->zoom
                               : fmin((W - 6) / w1, (H - 6) / h1);
        if (m.zoom < 0.05) m.zoom = 0.05;

        /* centre the projected bounding box */
        double Xc = (xmin + xmax) / 2.0 * m.zoom;
        double Yc = (ymin + ymax) / 2.0 * m.zoom;
        m.offsetX = W / 2.0 - Xc;
        m.offsetY = H / 2.0 - Yc;
    }

    /* --- ground grid and axes (drawn first, so the building covers them) -- */
    if (v->showGrid) {
        RGB grid = { UI_GRID_R, UI_GRID_G, UI_GRID_B };
        for (int gx = 0; gx <= r->size.x; gx++) {
            draw_line_3d(px, W, H, &m, gx, 0, 0, gx, 0, r->size.z, grid,
                         r->size.z * 3 + 4);
        }
        for (int gz = 0; gz <= r->size.z; gz++) {
            draw_line_3d(px, W, H, &m, 0, 0, gz, r->size.x, 0, gz, grid,
                         r->size.x * 3 + 4);
        }
    }
    if (v->showAxes) {
        RGB ax = { 226, 96, 96 }, ay = { 116, 206, 132 }, az = { 104, 150, 244 };
        double len = fmax(r->size.x, r->size.z) * 0.35 + 2.0;
        draw_line_3d(px, W, H, &m, 0, 0, 0, len, 0, 0, ax, 24);
        draw_line_3d(px, W, H, &m, 0, 0, 0, 0, len, 0, ay, 24);
        draw_line_3d(px, W, H, &m, 0, 0, 0, 0, 0, len, az, 24);
    }

    /* --- painter's algorithm --------------------------------------------- */
    size_t n = r->blockCount;
    double* depth = (double*)mb_arena_alloc(a, (n ? n : 1) * sizeof(double));
    size_t* order = (size_t*)mb_arena_alloc(a, (n ? n : 1) * sizeof(size_t));

    for (size_t i = 0; i < n; i++) {
        const MBVoxel* vx = &r->blocks[i];
        double x = vx->x + 0.5, z = vx->z + 0.5;
        double rx = (x - m.cx) * m.cosY - (z - m.cz) * m.sinY;
        double rz = (x - m.cx) * m.sinY + (z - m.cz) * m.cosY;
        /* larger = closer to the camera, which sits in the (+x, +y, +z) octant */
        depth[i] = rx + rz + (double)vx->y;
        order[i] = i;
    }

    /* insertion sort would be O(n^2); use a simple shell sort so that the
       dependency on qsort's stability does not matter. */
    for (size_t gap = n / 2; gap > 0; gap /= 2) {
        for (size_t i = gap; i < n; i++) {
            size_t tmp = order[i];
            double key = depth[tmp];
            size_t j = i;
            while (j >= gap && depth[order[j - gap]] > key) {
                order[j] = order[j - gap];
                j -= gap;
            }
            order[j] = tmp;
        }
    }

    double w = 0.8660254037844386 * m.zoom;
    double vv = 0.5 * m.depthScale * m.zoom;
    double cubeH = m.zoom;

    for (size_t k = 0; k < n; k++) {
        const MBVoxel* vx = &r->blocks[order[k]];
        double X, Y;
        iso_project(&m, vx->x + 0.5, (double)vx->y + 1.0, vx->z + 0.5, &X, &Y);
        draw_cube(px, W, H, X, Y, w, vv, cubeH, rgb_of(mb_material_color(vx->material)));
    }

    /* --- emit ------------------------------------------------------------ */
    {
        SBuf out;
        mb_sb_init(&out, a);
        mb_sb_puts(&out, "\x1b[0m");

        RGB lastFg = { 0, 0, 0 }, lastBg = { 0, 0, 0 };
        bool haveFg = false, haveBg = false;

        for (int row = 0; row < rows; row++) {
            for (int col = 0; col < W; col++) {
                RGB fg = px[(size_t)(row * 2) * (size_t)W + (size_t)col];
                RGB bgc = px[(size_t)(row * 2 + 1) * (size_t)W + (size_t)col];
                if (!haveFg || !rgb_eq(fg, lastFg)) { emit_fg(&out, fg); lastFg = fg; haveFg = true; }
                if (!haveBg || !rgb_eq(bgc, lastBg)) { emit_bg(&out, bgc); lastBg = bgc; haveBg = true; }
                mb_sb_puts(&out, "\xe2\x96\x80");     /* U+2580 UPPER HALF BLOCK */
            }
            mb_sb_puts(&out, "\x1b[0m\n");
            haveFg = haveBg = false;
        }
        return mb_sb_done(&out);
    }
}

/* ========================================================================== */
/* floor plan                                                                 */
/* ========================================================================== */

char* mb_render_slice(Arena* a, const MBBuildResult* r, int layer, int maxCols, int maxRows)
{
    int w = r->size.x;
    int h = r->size.z;
    if (w <= 0 || h <= 0) return mb_arena_strdup(a, "");

    /* Clip to what the caller can actually show. */
    int drawW = w > maxCols - 5 ? maxCols - 5 : w;
    int drawH = h > maxRows - 2 ? maxRows - 2 : h;
    if (drawW < 1) drawW = 1;
    if (drawH < 1) drawH = 1;

    const char** grid = (const char**)mb_arena_calloc(a, (size_t)drawW * (size_t)drawH *
                                                          sizeof(char*));
    for (size_t i = 0; i < r->blockCount; i++) {
        const MBVoxel* v = &r->blocks[i];
        if (v->y != layer) continue;
        if (v->x < 0 || v->x >= drawW || v->z < 0 || v->z >= drawH) continue;
        grid[(size_t)v->z * (size_t)drawW + (size_t)v->x] = v->material;
    }

    SBuf out;
    mb_sb_init(&out, a);

    RGB empty = { UI_VOID_R, UI_VOID_G, UI_VOID_B };
    RGB lastBg = { 0, 0, 0 };
    bool haveBg = false;

    for (int z = 0; z < drawH; z++) {
        mb_sb_printf(&out, "\x1b[0m\x1b[38;2;%d;%d;%dm%3d\x1b[0m ", UI_DIM_R, UI_DIM_G, UI_DIM_B, z);
        for (int x = 0; x < drawW; x++) {
            const char* mat = grid[(size_t)z * (size_t)drawW + (size_t)x];
            RGB c = mat ? rgb_of(mb_material_color(mat)) : empty;
            if (!haveBg || !rgb_eq(c, lastBg)) { emit_bg(&out, c); lastBg = c; haveBg = true; }
            mb_sb_puts(&out, " ");
        }
        mb_sb_puts(&out, "\x1b[0m\n");
        haveBg = false;
    }

    /* x axis ruler */
    mb_sb_printf(&out, "\x1b[0m\x1b[38;2;%d;%d;%dm    ", UI_DIM_R, UI_DIM_G, UI_DIM_B);
    for (int x = 0; x < drawW; x++) {
        if (x % 10 == 0) {
            char buf[24];
            snprintf(buf, sizeof(buf), "%-10d", x);
            mb_sb_puts(&out, buf);
        }
    }
    mb_sb_puts(&out, "\x1b[0m");

    return mb_sb_done(&out);
}

/* ========================================================================== */
/* legend                                                                     */
/* ========================================================================== */

/*
 * Every row is laid out to exactly 35 display columns: swatch(2) + name(8) +
 * bar(10) + count(5) + share(6) plus the single spaces between them.
 * That budget is what stops the panel from wrapping: a wide-graphic name is
 * clipped with mb_sb_puts_clip(), and the name field is then padded back out
 * with spaces using the *measured* width, never a printf field count (printf
 * counts bytes, and a single CJK glyph is three bytes for two cells).
 * main.c reserves at least 36 columns for the panel, so a row always fits.
 */
#define LEGEND_SWATCH   2   /* the coloured block marking the material   */
#define LEGEND_NAME     8   /* material name, clipped                    */
#define LEGEND_BAR     10   /* proportional bar                          */

static void legend_pad(SBuf* b, int cols)
{
    for (int i = 0; i < cols; i++) mb_sb_putc(b, ' ');
}

char* mb_render_legend(Arena* a, const MBBuildResult* r)
{
    SBuf out;
    mb_sb_init(&out, a);

    mb_sb_printf(&out, "\x1b[1m\x1b[38;2;%d;%d;%dm材料清单\x1b[0m"
                       "  \x1b[38;2;%d;%d;%dm%d 种 · %d 块\x1b[0m\n",
                 UI_SOFT_R, UI_SOFT_G, UI_SOFT_B,
                 UI_DIM_R, UI_DIM_G, UI_DIM_B, r->byMaterialCount, r->total);
    if (r->byMaterialCount == 0) {
        mb_sb_printf(&out, "\x1b[38;2;%d;%d;%dm（无方块）\x1b[0m\n",
                     UI_DIM_R, UI_DIM_G, UI_DIM_B);
        return mb_sb_done(&out);
    }

    /* One proportional bar shared by every row keeps the widest material
       visually comparable with the narrowest. */
    int widest = 1;
    for (int i = 0; i < r->byMaterialCount; i++) {
        if (r->byMaterial[i].count > widest) widest = r->byMaterial[i].count;
    }

    for (int i = 0; i < r->byMaterialCount; i++) {
        const MBMaterialUsage* u = &r->byMaterial[i];
        const MBMaterial* m = mb_material(u->id);
        RGB c = rgb_of(m->color);
        double pct = r->total > 0 ? ((double)u->count / (double)r->total) * 100.0 : 0.0;

        int filled = (int)((double)u->count / (double)widest * LEGEND_BAR + 0.5);
        if (filled < 1 && u->count > 0) filled = 1;
        if (filled > LEGEND_BAR) filled = LEGEND_BAR;

        /* swatch: a solid block of the material colour */
        mb_sb_printf(&out, "\x1b[38;2;%d;%d;%dm", c.r, c.g, c.b);
        for (int k = 0; k < LEGEND_SWATCH; k++) mb_sb_puts(&out, "\xe2\x96\x88");
        mb_sb_putc(&out, ' ');

        /* name, clipped and padded by *measured* columns */
        mb_sb_printf(&out, "\x1b[38;2;%d;%d;%dm", UI_SOFT_R, UI_SOFT_G, UI_SOFT_B);
        int nw = mb_sb_puts_clip(&out, m->name, strlen(m->name), LEGEND_NAME);
        legend_pad(&out, LEGEND_NAME - nw);
        mb_sb_putc(&out, ' ');

        /* bar */
        mb_sb_printf(&out, "\x1b[38;2;%d;%d;%dm", c.r, c.g, c.b);
        for (int k = 0; k < filled; k++) mb_sb_puts(&out, "\xe2\x96\x88");
        mb_sb_printf(&out, "\x1b[38;2;%d;%d;%dm", UI_GRID_R, UI_GRID_G, UI_GRID_B);
        for (int k = filled; k < LEGEND_BAR; k++) mb_sb_puts(&out, "\xe2\x96\x91");
        mb_sb_putc(&out, ' ');

        /* count + share */
        mb_sb_printf(&out, "\x1b[0m\x1b[38;2;%d;%d;%dm%5d\x1b[0m "
                           "\x1b[38;2;%d;%d;%dm%5.1f%%\x1b[0m\n",
                     UI_SOFT_R, UI_SOFT_G, UI_SOFT_B, u->count,
                     UI_DIM_R, UI_DIM_G, UI_DIM_B, pct);
    }
    return mb_sb_done(&out);
}
