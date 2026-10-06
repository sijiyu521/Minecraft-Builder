/*
 * gfx3d.c — the software 3D preview renderer.
 *
 * A small z-buffered rasteriser with no platform code in it at all: it takes a
 * camera and a build result and writes into a plain pixel buffer. That keeps it
 * drivable headlessly, which matters twice over — the regression tests can call
 * it directly, and `mb.exe --shot out.bmp` renders a frame to disk so the
 * preview can be checked without a window.
 *
 * What it draws, in order:
 *
 *   sky        a vertical gradient
 *   ground     one large dark quad, then two grids of thin quads on top of it
 *              (minor every block, major every eight)
 *   voxels     every face that is not hidden by a neighbouring block, shaded by
 *              orientation and lit by a fixed directional light
 *   glass      the transparent pass, sorted back to front and blended, drawn
 *              with depth testing but without depth writes
 *
 * Two details are worth knowing about:
 *
 *  - Faces are culled against a real occupancy hash, not just back-face culled.
 *    A solid 1342 block building exposes roughly 1300 faces instead of 8052,
 *    which is the difference between "interactive" and "slideshow".
 *
 *  - Every pixel is modulated by a hash of its quantised world position. That is
 *    what produces the grainy, slightly textured look: the pattern is welded to
 *    the surface, so it slides correctly instead of looking like screen noise.
 */
#include "mb.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Clip anything closer than this; a vertex at z = 0 would divide by zero. */
#define MB_NEAR 0.06

/* ========================================================================== */
/* vectors and colours                                                        */
/* ========================================================================== */

typedef struct { double x, y, z; } V3;

static int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static double clampd(double v, double lo, double hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static uint32_t rgb(int r, int g, int b)
{
    return ((uint32_t)clampi(r, 0, 255) << 16)
         | ((uint32_t)clampi(g, 0, 255) << 8)
         |  (uint32_t)clampi(b, 0, 255);
}

static int chan(uint32_t c, int shift) { return (int)((c >> shift) & 0xFFu); }

static uint32_t mix_rgb(uint32_t a, uint32_t b, double t)
{
    t = clampd(t, 0.0, 1.0);
    int r = (int)(chan(a, 16) + (chan(b, 16) - chan(a, 16)) * t + 0.5);
    int g = (int)(chan(a, 8)  + (chan(b, 8)  - chan(a, 8))  * t + 0.5);
    int bl = (int)(chan(a, 0) + (chan(b, 0)  - chan(a, 0))  * t + 0.5);
    return rgb(r, g, bl);
}

static uint32_t scale_rgb(uint32_t c, double k)
{
    return rgb((int)(chan(c, 16) * k), (int)(chan(c, 8) * k), (int)(chan(c, 0) * k));
}

static uint32_t blend_rgb(uint32_t dst, uint32_t src, double alpha)
{
    return mix_rgb(dst, src, alpha);
}

/* ========================================================================== */
/* framebuffer                                                                */
/* ========================================================================== */

static void fb_fill(MBFramebuffer* fb, uint32_t c)
{
    size_t n = (size_t)fb->width * (size_t)fb->height;
    for (size_t i = 0; i < n; i++) fb->color[i] = c;
}

void mb_fb_init(MBFramebuffer* fb, Arena* a, int width, int height)
{
    if (width  < 1) width  = 1;
    if (height < 1) height = 1;
    fb->width  = width;
    fb->height = height;
    fb->color = (uint32_t*)mb_arena_alloc(a, (size_t)width * (size_t)height * sizeof(uint32_t));
    fb->depth = (float*)mb_arena_alloc(a, (size_t)width * (size_t)height * sizeof(float));
    fb_fill(fb, 0);
}

void mb_fb_clear(MBFramebuffer* fb, uint32_t top, uint32_t bottom)
{
    for (int y = 0; y < fb->height; y++) {
        double t = fb->height > 1 ? (double)y / (double)(fb->height - 1) : 0.0;
        uint32_t c = mix_rgb(top, bottom, t);
        uint32_t* row = fb->color + (size_t)y * (size_t)fb->width;
        for (int x = 0; x < fb->width; x++) row[x] = c;
    }
    memset(fb->depth, 0, (size_t)fb->width * (size_t)fb->height * sizeof(float));
}

/* ========================================================================== */
/* camera                                                                     */
/* ========================================================================== */

/* Pitch is clamped just short of straight up/down: at the poles the basis would
   degenerate and the horizon would flip. */
static double clamp_pitch(double p)
{
    if (p >  1.45) return  1.45;
    if (p < -1.45) return -1.45;
    return p;
}

static void basis_of(const MBCamera* c, V3* right, V3* up, V3* fwd)
{
    double cp = cos(clamp_pitch(c->pitch));
    double sp = sin(clamp_pitch(c->pitch));
    double cy = cos(c->yaw);
    double sy = sin(c->yaw);

    V3 f = { cp * sy, sp, cp * cy };
    /* right = worldUp x forward, which keeps +X to the right when looking at +Z */
    V3 r = { f.z, 0.0, -f.x };
    double rl = sqrt(r.x * r.x + r.z * r.z);
    if (rl < 1e-9) { r.x = 1.0; r.z = 0.0; rl = 1.0; }
    r.x /= rl;
    r.z /= rl;
    V3 u = { f.y * r.z - f.z * r.y,
             f.z * r.x - f.x * r.z,
             f.x * r.y - f.y * r.x };

    *right = r;
    *up = u;
    *fwd = f;
}

void mb_camera_basis(const MBCamera* c, double* right, double* up, double* fwd)
{
    V3 r, u, f;
    basis_of(c, &r, &u, &f);
    if (right) { right[0] = r.x; right[1] = r.y; right[2] = r.z; }
    if (up)    { up[0]    = u.x; up[1]    = u.y; up[2]    = u.z; }
    if (fwd)   { fwd[0]   = f.x; fwd[1]   = f.y; fwd[2]   = f.z; }
}

void mb_camera_frame(MBCamera* c, const MBBuildResult* r)
{
    double sx = (double)r->size.x, sy = (double)r->size.y, sz = (double)r->size.z;
    double radius = 0.5 * sqrt(sx * sx + sy * sy + sz * sz);
    if (radius < 3.0) radius = 3.0;

    if (!(c->fov > 0.05)) c->fov = 0.9;
    double dist = radius / sin(c->fov * 0.5) * 1.02;

    V3 right, up, fwd;
    basis_of(c, &right, &up, &fwd);
    c->x = sx * 0.5 - fwd.x * dist;
    c->y = sy * 0.5 - fwd.y * dist;
    c->z = sz * 0.5 - fwd.z * dist;
}

void mb_camera_default(MBCamera* c, const MBBuildResult* r)
{
    memset(c, 0, sizeof(*c));
    c->yaw   = 0.62;      /* about 36 degrees */
    c->pitch = -0.40;     /* looking down at the roof */
    c->fov   = 0.90;
    mb_camera_frame(c, r);
}

void mb_scene_default(MBSceneOptions* o)
{
    o->showGrid   = true;
    o->showAxes   = false;
    o->autoOrbit  = false;
    o->gridExtent = 0.0;   /* 0 = derive from the building */
}

/* ========================================================================== */
/* procedural block textures                                                  */
/* ========================================================================== */
/*
 * There is no texture file anywhere in this project. Each material gets a 16x16
 * modulation map synthesised from its id the first time it is used, which is
 * how the preview ends up looking like Minecraft without shipping — or
 * licensing — a single pixel of anyone else's artwork.
 *
 * The values are multipliers, not colours: 128 means "leave the material colour
 * alone". They are stored per channel so a pattern can tint as well as darken
 * (leaves lean green, that sort of thing).
 */
#define MB_TEX     16
#define MB_TEX_MUL 128

typedef struct { unsigned char m[MB_TEX * MB_TEX * 3]; } MBTex;

static unsigned int mix32(unsigned int x)
{
    x ^= x >> 16; x *= 0x7FEB352Du;
    x ^= x >> 15; x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

/* Hash noise that wraps every `period` texels, so no pattern shows a seam
   where the texture repeats across adjacent blocks. */
static double value_noise(int x, int y, int period, unsigned int salt)
{
    int px = ((x % period) + period) % period;
    int py = ((y % period) + period) % period;
    unsigned int h = mix32((unsigned int)px * 0x9E3779B1u
                         ^ (unsigned int)py * 0x85EBCA6Bu ^ salt);
    return (double)(h & 0xFFFFu) / 65535.0;
}

/* Three octaves is enough to read as "stone" or "dirt" at 16x16. */
static double fbm(int x, int y, unsigned int salt)
{
    double a = value_noise(x, y, MB_TEX, salt);
    double b = value_noise(x / 2, y / 2, MB_TEX / 2, salt ^ 0xA5A5A5A5u);
    double c = value_noise(x / 4, y / 4, MB_TEX / 4, salt ^ 0x3C3C3C3Cu);
    return a * 0.55 + b * 0.30 + c * 0.15;
}

typedef enum {
    TK_STONE, TK_COBBLE, TK_PLANK, TK_LOG, TK_BRICK,
    TK_LEAF, TK_GRASS, TK_GLASS, TK_WOOL, TK_LIGHT, TK_METAL, TK_SOFT
} TexKind;

static TexKind tex_kind(const MBMaterial* m)
{
    const char* id = m->id;
    if (m->category == MB_CAT_GLASS)  return TK_GLASS;
    if (m->category == MB_CAT_LIGHT)  return TK_LIGHT;
    if (mb_contains_ci(id, "brick"))  return TK_BRICK;
    if (mb_contains_ci(id, "plank"))  return TK_PLANK;
    if (mb_contains_ci(id, "log") || mb_contains_ci(id, "wood")) return TK_LOG;
    if (mb_contains_ci(id, "cobble") || mb_contains_ci(id, "gravel") ||
        mb_contains_ci(id, "moss")   || mb_contains_ci(id, "clay")) return TK_COBBLE;
    if (mb_contains_ci(id, "leav"))   return TK_LEAF;
    if (mb_contains_ci(id, "grass"))  return TK_GRASS;
    if (mb_contains_ci(id, "wool") || mb_contains_ci(id, "carpet")) return TK_WOOL;
    if (mb_contains_ci(id, "iron") || mb_contains_ci(id, "gold") ||
        mb_contains_ci(id, "copper") || mb_contains_ci(id, "metal")) return TK_METAL;
    if (m->category == MB_CAT_STONE)  return TK_STONE;
    if (m->category == MB_CAT_WOOL)   return TK_WOOL;
    if (m->category == MB_CAT_NATURE) return TK_LEAF;
    return TK_SOFT;
}

static void tex_gen(MBTex* t, const MBMaterial* m)
{
    TexKind kind = tex_kind(m);
    unsigned int salt = mb_hash_string(m->id);

    for (int y = 0; y < MB_TEX; y++) {
        for (int x = 0; x < MB_TEX; x++) {
            double k = 1.0, tr = 1.0, tg = 1.0, tb = 1.0;
            double n = fbm(x, y, salt);

            switch (kind) {
            case TK_STONE:
                k = 0.88 + n * 0.24;
                if (value_noise(x, y, MB_TEX, salt ^ 0x11u) > 0.93) k *= 0.82;
                break;

            case TK_COBBLE: {
                int cx = x / 4, cy = y / 4;
                double cell = 0.86 + value_noise(cx, cy, 4, salt) * 0.30;
                double seam = (x % 4 == 0 || y % 4 == 0) ? 0.76 : 1.0;
                k = cell * seam * (0.95 + n * 0.10);
                break;
            }

            case TK_PLANK: {
                int row = y / 4;
                double seam  = (y % 4 == 0) ? 0.74 : 1.0;
                double joint = ((x + row * 7) % 8 == 0) ? 0.82 : 1.0;
                k = seam * joint * (0.94 + value_noise(x, y / 2, MB_TEX, salt) * 0.13);
                break;
            }

            case TK_LOG: {
                /* constant along the grain axis (v == y), varying across it */
                double rings = 0.90 + value_noise(x, 0, MB_TEX, salt) * 0.20;
                double bite  = 0.96 + value_noise(x * 3, y / 4, MB_TEX, salt ^ 0x7Bu) * 0.08;
                k = rings * bite;
                break;
            }

            case TK_BRICK: {
                int row = y / 4;
                int bx  = (x + row * 4) % 8;
                double mortar = (y % 4 == 0 || bx == 0) ? 0.70 : 1.0;
                k = mortar * (0.94 + n * 0.14);
                break;
            }

            case TK_LEAF:
                /* clumps with gaps between them, the way foliage reads */
                if (n < 0.30) k = 0.52 + n * 0.9;
                else          k = 0.86 + n * 0.34;
                tr = 0.94; tg = 1.08; tb = 0.90;
                break;

            case TK_GRASS:
                k = 0.88 + n * 0.26;
                tr = 0.96; tg = 1.06; tb = 0.92;
                break;

            case TK_GLASS: {
                int band = (x + y) % MB_TEX;
                double hi = band < 3 ? 1.30 : 1.0;
                k = (0.98 + value_noise(x, y, MB_TEX, salt) * 0.04) * hi;
                break;
            }

            case TK_WOOL:
                k = 0.93 + value_noise(x / 2, y / 2, 8, salt) * 0.14;
                break;

            case TK_LIGHT: {
                double dx = (x + 0.5 - MB_TEX * 0.5) / (MB_TEX * 0.5);
                double dy = (y + 0.5 - MB_TEX * 0.5) / (MB_TEX * 0.5);
                k = 1.55 - sqrt(dx * dx + dy * dy) * 0.55;
                break;
            }

            case TK_METAL:
                k = 1.02 + value_noise(x, y / 3, MB_TEX, salt) * 0.07;
                break;

            case TK_SOFT:
            default:
                k = 0.94 + n * 0.13;
                break;
            }

            /* a whisper of per-texel jitter so nothing is perfectly flat */
            k *= 0.98 + value_noise(x, y, MB_TEX, salt ^ 0xDEADu) * 0.05;

            int i = (y * MB_TEX + x) * 3;
            t->m[i + 0] = (unsigned char)clampi((int)(MB_TEX_MUL * k * tr + 0.5), 0, 255);
            t->m[i + 1] = (unsigned char)clampi((int)(MB_TEX_MUL * k * tg + 0.5), 0, 255);
            t->m[i + 2] = (unsigned char)clampi((int)(MB_TEX_MUL * k * tb + 0.5), 0, 255);
        }
    }
}

static double pick_axis(double x, double y, double z, int axis)
{
    return axis == 0 ? x : (axis == 1 ? y : z);
}

/*
 * Fetch a texel and apply it to the material colour. The block face outline is
 * folded in here as well: darkening the last eighth of a block gives every cube
 * a readable edge at a glance, which matters a lot at preview resolutions.
 */
static uint32_t tex_shade(const MBTex* t, uint32_t base, int uvA, int uvB,
                          double wx, double wy, double wz)
{
    double u = pick_axis(wx, wy, wz, uvA);
    double v = pick_axis(wx, wy, wz, uvB);

    int tx = (int)floor(u * MB_TEX);
    int ty = (int)floor((1.0 - v) * MB_TEX);
    tx = ((tx % MB_TEX) + MB_TEX) % MB_TEX;
    ty = ((ty % MB_TEX) + MB_TEX) % MB_TEX;

    const unsigned char* m = t->m + (ty * MB_TEX + tx) * 3;
    double kr = m[0] / (double)MB_TEX_MUL;
    double kg = m[1] / (double)MB_TEX_MUL;
    double kb = m[2] / (double)MB_TEX_MUL;

    double fu = u - floor(u), fv = v - floor(v);
    double eu = fu < 0.5 ? fu : 1.0 - fu;
    double ev = fv < 0.5 ? fv : 1.0 - fv;
    double e  = eu < ev ? eu : ev;
    if (e < 0.09) {
        double edge = 0.80 + 0.20 * (e / 0.09);
        kr *= edge; kg *= edge; kb *= edge;
    }

    return rgb((int)(chan(base, 16) * kr + 0.5),
               (int)(chan(base, 8)  * kg + 0.5),
               (int)(chan(base, 0)  * kb + 0.5));
}

/* ========================================================================== */
/* rasteriser                                                                 */
/* ========================================================================== */

/* A vertex on its way through the pipeline. It carries the world position as
   well as the view position because the per pixel grain is keyed off world
   space — that is what makes the texture stick to the surface. */
typedef struct {
    double   vx, vy, vz;    /* view space */
    double   wx, wy, wz;    /* world space */
    uint32_t color;
    int      textured;
} SV;

typedef struct {
    const MBCamera* cam;
    V3     right, up, fwd;
    double scale;          /* pixels per unit at unit distance */
    double cx, cy;         /* projection centre */
    bool   writeDepth;
    double alpha;
    /* distance fog, so the ground plane dissolves into the horizon instead of
       ending in a hard edge the way an infinite plane would */
    double fogStart, fogEnd;
    uint32_t fogColor;
    /* set per face by the caller: which world axes span the surface, and which
       texture to read. tex == NULL means "flat colour", used by ground/axes. */
    const MBTex* tex;
    int    uvA, uvB;
} Raster;

static void raster_init(Raster* R, const MBCamera* cam, const MBFramebuffer* fb,
                        bool writeDepth, double alpha)
{
    memset(R, 0, sizeof(*R));
    R->cam = cam;
    basis_of(cam, &R->right, &R->up, &R->fwd);
    double fov = cam->fov > 0.05 ? cam->fov : 0.9;
    R->scale = (fb->height * 0.5) / tan(fov * 0.5);
    R->cx = fb->width * 0.5;
    R->cy = fb->height * 0.5;
    R->writeDepth = writeDepth;
    R->alpha = alpha;
    R->fogStart = 1e30;
    R->fogEnd = 1e30;
    R->fogColor = 0;
    R->tex = NULL;
    R->uvA = 0;
    R->uvB = 1;
}

static void sv_set(const Raster* R, SV* v, double wx, double wy, double wz,
                   uint32_t color, int textured)
{
    double dx = wx - R->cam->x;
    double dy = wy - R->cam->y;
    double dz = wz - R->cam->z;
    v->wx = wx; v->wy = wy; v->wz = wz;
    v->vx = dx * R->right.x + dy * R->right.y + dz * R->right.z;
    v->vy = dx * R->up.x    + dy * R->up.y    + dz * R->up.z;
    v->vz = dx * R->fwd.x   + dy * R->fwd.y   + dz * R->fwd.z;
    v->color = color;
    v->textured = textured;
}

static void sv_lerp(const SV* a, const SV* b, double t, SV* out)
{
    out->vx = a->vx + (b->vx - a->vx) * t;
    out->vy = a->vy + (b->vy - a->vy) * t;
    out->vz = a->vz + (b->vz - a->vz) * t;
    out->wx = a->wx + (b->wx - a->wx) * t;
    out->wy = a->wy + (b->wy - a->wy) * t;
    out->wz = a->wz + (b->wz - a->wz) * t;
    out->color = a->color;
    out->textured = a->textured;
}

/*
 * Sutherland-Hodgman against the near plane. Without this a triangle straddling
 * the camera plane projects to a bow tie and smears across the screen.
 */
static int clip_near(const SV* in, int n, SV* out)
{
    int m = 0;
    for (int i = 0; i < n; i++) {
        const SV* a = &in[i];
        const SV* b = &in[(i + 1) % n];
        double da = a->vz - MB_NEAR;
        double db = b->vz - MB_NEAR;
        if (da >= 0.0) out[m++] = *a;
        if ((da >= 0.0) != (db >= 0.0)) {
            sv_lerp(a, b, da / (da - db), &out[m++]);
        }
    }
    return m;
}

static void plot(MBFramebuffer* fb, const Raster* R, int px, int py,
                 double invz, double wx, double wy, double wz, uint32_t color, int textured)
{
    size_t idx = (size_t)py * (size_t)fb->width + (size_t)px;
    if (!(invz > (double)fb->depth[idx])) return;
    if (R->writeDepth) fb->depth[idx] = (float)invz;

    uint32_t c = (textured && R->tex) ? tex_shade(R->tex, color, R->uvA, R->uvB, wx, wy, wz)
                                      : color;

    if (R->fogEnd > R->fogStart) {
        double z = 1.0 / invz;
        double t = (z - R->fogStart) / (R->fogEnd - R->fogStart);
        if (t > 0.0) {
            if (t > 0.85) t = 0.85;     /* keep a hint of the material at the far edge */
            c = mix_rgb(c, R->fogColor, t);
        }
    }

    if (R->alpha < 0.999) c = blend_rgb(fb->color[idx], c, R->alpha);
    fb->color[idx] = c;
}

static void fill_tri(MBFramebuffer* fb, const Raster* R,
                     const SV* a, const SV* b, const SV* c)
{
    double ax = R->cx + a->vx * R->scale / a->vz;
    double ay = R->cy - a->vy * R->scale / a->vz;
    double bx = R->cx + b->vx * R->scale / b->vz;
    double by = R->cy - b->vy * R->scale / b->vz;
    double cx = R->cx + c->vx * R->scale / c->vz;
    double cy = R->cy - c->vy * R->scale / c->vz;

    double area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (area > -1e-9 && area < 1e-9) return;

    int minx = (int)floor(ax < bx ? (ax < cx ? ax : cx) : (bx < cx ? bx : cx));
    int maxx = (int)ceil (ax > bx ? (ax > cx ? ax : cx) : (bx > cx ? bx : cx));
    int miny = (int)floor(ay < by ? (ay < cy ? ay : cy) : (by < cy ? by : cy));
    int maxy = (int)ceil (ay > by ? (ay > cy ? ay : cy) : (by > cy ? by : cy));

    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > fb->width  - 1) maxx = fb->width  - 1;
    if (maxy > fb->height - 1) maxy = fb->height - 1;
    if (minx > maxx || miny > maxy) return;

    double iwa = 1.0 / a->vz, iwb = 1.0 / b->vz, iwc = 1.0 / c->vz;
    double inv_area = 1.0 / area;

    for (int py = miny; py <= maxy; py++) {
        double fy = (double)py + 0.5;
        for (int px = minx; px <= maxx; px++) {
            double fx = (double)px + 0.5;

            /* barycentric via edge functions; the weights sum to 1 by
               construction, so testing them against zero is the whole test. */
            double e0 = ((cx - bx) * (fy - by) - (cy - by) * (fx - bx)) * inv_area;
            double e1 = ((ax - cx) * (fy - cy) - (ay - cy) * (fx - cx)) * inv_area;
            double e2 = ((bx - ax) * (fy - ay) - (by - ay) * (fx - ax)) * inv_area;
            if (e0 < 0.0 || e1 < 0.0 || e2 < 0.0) continue;

            double invz = e0 * iwa + e1 * iwb + e2 * iwc;
            if (!(invz > 0.0)) continue;

            /* perspective correct: divide the weighted attributes by the
               interpolated 1/z before reading them back to screen space. */
            double z = 1.0 / invz;
            double wx = (e0 * a->wx * iwa + e1 * b->wx * iwb + e2 * c->wx * iwc) * z;
            double wy = (e0 * a->wy * iwa + e1 * b->wy * iwb + e2 * c->wy * iwc) * z;
            double wz = (e0 * a->wz * iwa + e1 * b->wz * iwb + e2 * c->wz * iwc) * z;

            plot(fb, R, px, py, invz, wx, wy, wz, a->color, a->textured);
        }
    }
}

/* Clip a convex polygon, then fan triangulate into the rasteriser. */
static void emit_poly(MBFramebuffer* fb, const Raster* R, const SV* poly, int n)
{
    SV clipped[8];
    int m = clip_near(poly, n, clipped);
    for (int i = 2; i < m; i++) fill_tri(fb, R, &clipped[0], &clipped[i - 1], &clipped[i]);
}

static void emit_quad(MBFramebuffer* fb, const Raster* R,
                      double x0, double y0, double z0,
                      double x1, double y1, double z1,
                      double x2, double y2, double z2,
                      double x3, double y3, double z3,
                      uint32_t color, int textured)
{
    SV q[4];
    sv_set(R, &q[0], x0, y0, z0, color, textured);
    sv_set(R, &q[1], x1, y1, z1, color, textured);
    sv_set(R, &q[2], x2, y2, z2, color, textured);
    sv_set(R, &q[3], x3, y3, z3, color, textured);
    emit_poly(fb, R, q, 4);
}

/* ========================================================================== */
/* ground                                                                     */
/* ========================================================================== */

static void draw_ground(MBFramebuffer* fb, const Raster* R,
                        double cx, double cz, double extent)
{
    uint32_t plane = rgb(0x33, 0x36, 0x2A);
    uint32_t minor = rgb(0x43, 0x48, 0x39);
    uint32_t major = rgb(0x5C, 0x64, 0x50);

    double x0 = cx - extent, x1 = cx + extent;
    double z0 = cz - extent, z1 = cz + extent;

    emit_quad(fb, R, x0, 0, z0, x1, 0, z0, x1, 0, z1, x0, 0, z1, plane, 0);

    /* Two passes rather than one coloured line: the major lines are drawn on a
       slightly higher plane so they win the depth test where they cross. */
    const double lift = 0.012;
    const double w = 0.030;

    for (int pass = 0; pass < 2; pass++) {
        int step = pass == 0 ? 1 : 8;
        uint32_t col = pass == 0 ? minor : major;
        double y = lift * (pass + 1);

        long i0 = (long)floor((-extent) / step);
        long i1 = (long)ceil((extent) / step);
        for (long i = i0; i <= i1; i++) {
            double gx = cx + (double)i * step;
            double gz = cz + (double)i * step;
            if (gx < x0 - step || gx > x1 + step) continue;
            if (gz < z0 - step || gz > z1 + step) continue;
            /* line parallel to Z at x = gx */
            emit_quad(fb, R, gx - w, y, z0, gx + w, y, z0,
                                gx + w, y, z1, gx - w, y, z1, col, 0);
            /* line parallel to X at z = gz */
            emit_quad(fb, R, x0, y, gz - w, x1, y, gz - w,
                                x1, y, gz + w, x0, y, gz + w, col, 0);
        }
    }
}

static void draw_axes(MBFramebuffer* fb, const Raster* R, double len)
{
    if (len < 3.0) len = 3.0;
    len *= 0.35;
    const double t = 0.06;
    uint32_t ax = rgb(0xD0, 0x4E, 0x4E);
    uint32_t ay = rgb(0x6E, 0xC8, 0x64);
    uint32_t az = rgb(0x5A, 0x8A, 0xEB);

    emit_quad(fb, R, 0, -t, -t, len, -t, -t, len, t, t, 0, t, t, ax, 0);
    emit_quad(fb, R, -t, 0, -t, len, 0, -t, len, 0, t, -t, 0, t, ax, 0);
    emit_quad(fb, R, -t, -t, 0, -t, -t, len, t, t, len, t, t, 0, az, 0);
    emit_quad(fb, R, -t, 0, 0, -t, 0, len, t, 0, len, t, 0, 0, az, 0);
    emit_quad(fb, R, -t, 0, -t, t, 0, -t, t, len, t, -t, len, t, ay, 0);
    emit_quad(fb, R, -t, 0, t, t, 0, t, t, len, -t, -t, len, -t, ay, 0);
}

/* ========================================================================== */
/* voxels                                                                     */
/* ========================================================================== */

static const int FACE_DX[6] = {  1, -1,  0,  0,  0,  0 };
static const int FACE_DY[6] = {  0,  0,  1, -1,  0,  0 };
static const int FACE_DZ[6] = {  0,  0,  0,  0,  1, -1 };

static const double FACE_K[6] = { 0.72, 0.64, 0.94, 0.40, 0.84, 0.56 };

static const V3 FACE_N[6] = {
    {  1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }
};

static const int FACE_VX[6][4] = {
    { 1, 1, 1, 1 }, { 0, 0, 0, 0 }, { 0, 1, 1, 0 },
    { 0, 1, 1, 0 }, { 1, 0, 0, 1 }, { 0, 1, 1, 0 }
};
static const int FACE_VY[6][4] = {
    { 0, 0, 1, 1 }, { 0, 0, 1, 1 }, { 1, 1, 1, 1 },
    { 0, 0, 0, 0 }, { 0, 0, 1, 1 }, { 0, 0, 1, 1 }
};
static const int FACE_VZ[6][4] = {
    { 0, 1, 1, 0 }, { 1, 0, 0, 1 }, { 0, 0, 1, 1 },
    { 1, 1, 0, 0 }, { 1, 1, 0, 0 }, { 0, 0, 1, 1 }
};

#define MAX_PALETTE 64

typedef struct {
    const char* id;
    uint32_t    color;
    const MBTex* tex;
    bool        glass;
    bool        light;
} PalEnt;

typedef struct {
    PalEnt ent[MAX_PALETTE];
    int    count;
    MBTex* pool;          /* one synthesised texture per palette slot */
} ScenePal;

static int pal_slot(ScenePal* p, const char* id)
{
    /* The build hands out material strings from the arena, so most of the time
       the pointer is already the one we stored. */
    for (int i = 0; i < p->count; i++) if (p->ent[i].id == id) return i;
    for (int i = 0; i < p->count; i++) if (strcmp(p->ent[i].id, id) == 0) return i;
    if (p->count >= MAX_PALETTE) return 0;

    const MBMaterial* m = mb_material(id);
    int idx = p->count++;
    PalEnt* e = &p->ent[idx];
    e->id    = id;
    e->color = m->color;
    e->tex   = &p->pool[idx];
    e->glass = (m->category == MB_CAT_GLASS);
    e->light = (m->light != 0);
    tex_gen(&p->pool[idx], m);
    return idx;
}

/*
 * Occupancy hash. A dense 3D array would be 48*40*48 bytes for the largest
 * building; a hash of the coordinates costs one probe per face instead.
 */
typedef struct {
    uint32_t* slot;   /* 0 = empty, otherwise the palette index + 1 */
    uint32_t* keys;   /* packed coordinate, meaningful when slot != 0 */
    size_t    cap;
} VoxSet;

static uint32_t pack_xyz(int x, int y, int z)
{
    return ((uint32_t)x & 0x3FFu)
         | (((uint32_t)y & 0x3FFu) << 10)
         | (((uint32_t)z & 0x3FFu) << 20)
         | 0x40000000u;             /* keeps a real key from ever being zero */
}

static void vox_init(VoxSet* s, Arena* a, size_t want)
{
    size_t cap = 64;
    while (cap < want * 4 + 8) cap <<= 1;
    s->cap  = cap;
    s->slot = (uint32_t*)mb_arena_calloc(a, cap * sizeof(uint32_t));
    s->keys = (uint32_t*)mb_arena_calloc(a, cap * sizeof(uint32_t));
}

static size_t vox_start(const VoxSet* s, uint32_t key)
{
    uint32_t h = key * 2654435761u;
    h ^= h >> 15;
    return (size_t)h & (s->cap - 1);
}

static void vox_put(VoxSet* s, int x, int y, int z, uint32_t val)
{
    uint32_t key = pack_xyz(x, y, z);
    size_t i = vox_start(s, key);
    while (s->slot[i] != 0 && s->keys[i] != key) i = (i + 1) & (s->cap - 1);
    s->keys[i] = key;
    s->slot[i] = val;
}

static uint32_t vox_get(const VoxSet* s, int x, int y, int z)
{
    uint32_t key = pack_xyz(x, y, z);
    size_t i = vox_start(s, key);
    for (;;) {
        if (s->slot[i] == 0) return 0;
        if (s->keys[i] == key) return s->slot[i];
        i = (i + 1) & (s->cap - 1);
    }
}

/* A fixed light from above and behind the default camera. */
static const V3 LIGHT = { 0.453, 0.805, 0.383 };

static uint32_t shade_face(uint32_t base, int face, bool emissive)
{
    double d = FACE_N[face].x * LIGHT.x + FACE_N[face].y * LIGHT.y + FACE_N[face].z * LIGHT.z;
    if (d < 0.0) d = 0.0;
    double k = FACE_K[face] * (0.78 + 0.30 * d);
    if (emissive) k *= 1.30;
    if (k > 1.55) k = 1.55;
    return scale_rgb(base, k);
}

/* Which world axes span face `f`: +-X faces are cut from the YZ plane, tops and
   bottoms from XZ, and +-Z from XY. Getting this right is what makes the wood
   grain run down the side of a log instead of across it. */
static const int FACE_UVA[6] = { 2, 2, 0, 0, 0, 0 };
static const int FACE_UVB[6] = { 1, 1, 2, 2, 1, 1 };

static void block_faces(MBFramebuffer* fb, Raster* R, const MBVoxel* v,
                        int idx, const ScenePal* pal, const VoxSet* occ)
{
    const PalEnt* pe = &pal->ent[idx];
    double bx = v->x, by = v->y, bz = v->z;
    R->tex  = pe->tex;
    R->uvA  = FACE_UVA[0];
    R->uvB  = FACE_UVB[0];

    for (int f = 0; f < 6; f++) {
        /* back-face cull: the normal points away from the camera */
        if (FACE_N[f].x * R->fwd.x + FACE_N[f].y * R->fwd.y + FACE_N[f].z * R->fwd.z >= 0.0) {
            continue;
        }

        uint32_t nb = vox_get(occ, v->x + FACE_DX[f], v->y + FACE_DY[f], v->z + FACE_DZ[f]);
        if (nb != 0) {
            /* A glass block is hidden by any neighbour; an opaque one only by an
               opaque neighbour. */
            if (pe->glass) continue;
            if (!pal->ent[nb - 1].glass) continue;
        }

        uint32_t col = shade_face(pe->color, f, pe->light);
        double px[4], py[4], pz[4];
        for (int k = 0; k < 4; k++) {
            px[k] = bx + FACE_VX[f][k];
            py[k] = by + FACE_VY[f][k];
            pz[k] = bz + FACE_VZ[f][k];
        }
        R->uvA = FACE_UVA[f];
        R->uvB = FACE_UVB[f];
        emit_quad(fb, R, px[0], py[0], pz[0], px[1], py[1], pz[1],
                        px[2], py[2], pz[2], px[3], py[3], pz[3], col, 1);
    }

    R->tex = NULL;   /* leave the shared state clean for the next caller */
}

/* ========================================================================== */
/* scene                                                                      */
/* ========================================================================== */

void mb_render_scene(Arena* a, MBFramebuffer* fb, const MBBuildResult* r,
                     const MBCamera* cam, const MBSceneOptions* opt)
{
    mb_fb_clear(fb, rgb(0x10, 0x12, 0x15), rgb(0x2A, 0x2C, 0x24));

    Raster solid;
    raster_init(&solid, cam, fb, true, 1.0);

    double extent = opt->gridExtent;
    if (!(extent > 0.0)) {
        double m = r->size.x > r->size.z ? (double)r->size.x : (double)r->size.z;
        extent = m * 2.5 + 24.0;
        if (extent < 40.0) extent = 40.0;
    }

    /* Fog is measured from the camera to the centre of the building, so it never
       touches the model no matter how the camera is positioned. */
    {
        double cxm = r->size.x * 0.5, cym = r->size.y * 0.5, czm = r->size.z * 0.5;
        double dx = cam->x - cxm, dy = cam->y - cym, dz = cam->z - czm;
        double d = sqrt(dx * dx + dy * dy + dz * dz);
        if (d < 1.0) d = 1.0;
        double hue = r->size.x > r->size.z ? (double)r->size.x : (double)r->size.z;
        solid.fogStart = d + hue * 0.6;
        solid.fogEnd   = solid.fogStart + extent * 1.6;
        solid.fogColor = rgb(0x26, 0x28, 0x20);
    }

    if (opt->showGrid) {
        draw_ground(fb, &solid, r->size.x * 0.5, r->size.z * 0.5, extent);
    }
    if (opt->showAxes) {
        double l = r->size.x > r->size.z ? (double)r->size.x : (double)r->size.z;
        draw_axes(fb, &solid, l);
    }

    if (r->blockCount == 0) return;

    ScenePal pal;
    pal.count = 0;
    pal.pool  = (MBTex*)mb_arena_alloc(a, MAX_PALETTE * sizeof(MBTex));

    int* midx = (int*)mb_arena_alloc(a, r->blockCount * sizeof(int));
    VoxSet occ;
    vox_init(&occ, a, r->blockCount);

    for (size_t i = 0; i < r->blockCount; i++) {
        midx[i] = pal_slot(&pal, r->blocks[i].material);
        vox_put(&occ, r->blocks[i].x, r->blocks[i].y, r->blocks[i].z, (uint32_t)midx[i] + 1u);
    }

    /* --- opaque pass ------------------------------------------------------ */
    for (size_t i = 0; i < r->blockCount; i++) {
        if (pal.ent[midx[i]].glass) continue;
        block_faces(fb, &solid, &r->blocks[i], midx[i], &pal, &occ);
    }

    /* --- transparent pass ------------------------------------------------- */
    /* Blending is order dependent, so the glass blocks go back to front. */
    size_t glassCount = 0;
    for (size_t i = 0; i < r->blockCount; i++) if (pal.ent[midx[i]].glass) glassCount++;
    if (glassCount == 0) return;

    size_t* order = (size_t*)mb_arena_alloc(a, glassCount * sizeof(size_t));
    double* dist  = (double*)mb_arena_alloc(a, glassCount * sizeof(double));
    size_t n = 0;
    for (size_t i = 0; i < r->blockCount; i++) {
        if (!pal.ent[midx[i]].glass) continue;
        double dx = r->blocks[i].x + 0.5 - cam->x;
        double dy = r->blocks[i].y + 0.5 - cam->y;
        double dz = r->blocks[i].z + 0.5 - cam->z;
        dist[n]  = dx * solid.fwd.x + dy * solid.fwd.y + dz * solid.fwd.z;
        order[n] = i;
        n++;
    }

    /* insertion sort: the glass count is small and this keeps the order stable */
    for (size_t i = 1; i < n; i++) {
        size_t key = order[i];
        double kd = dist[i];
        size_t j = i;
        while (j > 0 && dist[j - 1] < kd) {
            order[j] = order[j - 1];
            dist[j] = dist[j - 1];
            j--;
        }
        order[j] = key;
        dist[j] = kd;
    }

    /*
     * The glass pass writes depth as well. Without that, every pane along the
     * view ray blends again and a wall of glass turns almost black, because
     * each layer keeps multiplying the same tint over the last one. Letting the
     * nearest pane claim the pixel keeps a facade looking like a single sheet.
     */
    Raster glass;
    raster_init(&glass, cam, fb, true, 0.34);
    glass.fogStart = solid.fogStart;
    glass.fogEnd = solid.fogEnd;
    glass.fogColor = solid.fogColor;
    for (size_t k = 0; k < n; k++) {
        size_t i = order[k];
        block_faces(fb, &glass, &r->blocks[i], midx[i], &pal, &occ);
    }
}

/* ========================================================================== */
/* BMP output                                                                 */
/* ========================================================================== */

static void put_u16(unsigned char* p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void put_u32(unsigned char* p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static size_t bmp_row_stride(const MBFramebuffer* fb)
{
    size_t row = (size_t)fb->width * 3u;
    return (row + 3u) & ~(size_t)3u;      /* BMP rows are padded to 4 bytes */
}

size_t mb_bmp_size(const MBFramebuffer* fb)
{
    return 54u + bmp_row_stride(fb) * (size_t)fb->height;
}

void mb_bmp_encode(const MBFramebuffer* fb, unsigned char* out)
{
    size_t stride = bmp_row_stride(fb);
    size_t dataSize = stride * (size_t)fb->height;

    memset(out, 0, 54);
    out[0] = 'B';
    out[1] = 'M';
    put_u32(out + 2,  (unsigned)(54u + dataSize));
    put_u32(out + 10, 54u);
    put_u32(out + 14, 40u);
    put_u32(out + 18, (unsigned)fb->width);
    put_u32(out + 22, (unsigned)fb->height);
    put_u16(out + 26, 1u);
    put_u16(out + 28, 24u);
    put_u32(out + 34, (unsigned)dataSize);
    put_u32(out + 38, 2835u);              /* 72 dpi */
    put_u32(out + 42, 2835u);

    /* bottom-up: the last scanline of the image is written first */
    for (int y = 0; y < fb->height; y++) {
        unsigned char* dst = out + 54u + (size_t)(fb->height - 1 - y) * stride;
        const uint32_t* src = fb->color + (size_t)y * (size_t)fb->width;
        for (int x = 0; x < fb->width; x++) {
            uint32_t c = src[x];
            dst[x * 3 + 0] = (unsigned char)chan(c, 0);
            dst[x * 3 + 1] = (unsigned char)chan(c, 8);
            dst[x * 3 + 2] = (unsigned char)chan(c, 16);
        }
    }
}

bool mb_bmp_write(const MBFramebuffer* fb, const char* path)
{
    size_t n = mb_bmp_size(fb);
    unsigned char* buf = (unsigned char*)malloc(n);
    if (!buf) return false;
    mb_bmp_encode(fb, buf);

    FILE* f = fopen(path, "wb");
    bool ok = f != NULL && fwrite(buf, 1, n, f) == n;
    if (f) fclose(f);
    free(buf);
    return ok;
}
