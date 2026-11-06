/*
 * textures.c — optional vanilla Minecraft block textures.
 *
 * Minecraft's textures are Mojang assets and are NOT part of this repository.
 * The software renderer ships a procedural fallback that needs no art; if the
 * user extracts their own copy of the vanilla block textures (the helper in
 * tools/extract-vanilla-textures.ps1 does that from their installed .minecraft),
 * this module loads those 16x16 PNGs at runtime and the renderer prefers them.
 *
 * Search order:
 *   1. an explicit directory (mb_textures_set_dir / --textures)
 *   2. $MB_TEXTURES
 *   3. <exe>/textures, <exe>/../textures
 *   4. <cwd>/textures
 *   5. %APPDATA%\.minecraft\resourcepacks\*\assets\minecraft\textures\block
 *
 * A pack root, a "textures" folder, or the "block" folder itself are all
 * accepted. PNG decoding uses the Windows Imaging Component, so the rest of
 * the project stays portable: non-Windows builds simply keep the procedural
 * textures.
 */
#include "mb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEX_SIZE 16
#define TEX_DIR_CAP 512

static char g_tex_dir[TEX_DIR_CAP];
static int  g_tex_detected;          /* 0 = not tried, 1 = done */
static int  g_tex_ready;             /* cached "directory has block PNGs" */

#ifdef _WIN32
#include <windows.h>
#include <wincodec.h>

static bool file_exists(const char* path)
{
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}
#else
static bool file_exists(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}
#endif

static void path_join(char* out, size_t cap, const char* a, const char* b)
{
    if (!a || !*a) { snprintf(out, cap, "%s", b ? b : ""); return; }
    size_t n = strlen(a);
    bool slash = n > 0 && (a[n - 1] == '/' || a[n - 1] == '\\');
    snprintf(out, cap, "%s%s%s", a, slash ? "" : "/", b ? b : "");
}

/* A directory counts as a block-texture folder when a couple of the vanilla
 * filenames exist. This avoids accepting an arbitrary folder of PNGs. */
static bool is_block_dir(const char* dir)
{
    if (!dir || !*dir) return false;
    static const char* const probes[] = {
        "stone.png", "oak_planks.png", "grass_block_top.png", "white_concrete.png"
    };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        char path[TEX_DIR_CAP + 64];
        path_join(path, sizeof(path), dir, probes[i]);
        if (file_exists(path)) return true;
    }
    return false;
}

/* Accepts a resource-pack root, a textures folder, or the block folder. */
static bool resolve_block_dir(const char* root, char* out, size_t cap)
{
    static const char* const suffixes[] = {
        "", "assets/minecraft/textures/block", "textures/block", "block"
    };
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        char dir[TEX_DIR_CAP];
        if (!suffixes[i][0]) snprintf(dir, sizeof(dir), "%s", root);
        else path_join(dir, sizeof(dir), root, suffixes[i]);
        if (is_block_dir(dir)) {
            snprintf(out, cap, "%s", dir);
            return true;
        }
    }
    return false;
}

bool mb_textures_set_dir(const char* dir)
{
    g_tex_detected = 1;
    g_tex_ready = 0;
    g_tex_dir[0] = '\0';
    if (!dir || !*dir) return false;
    char resolved[TEX_DIR_CAP];
    if (!resolve_block_dir(dir, resolved, sizeof(resolved))) return false;
    snprintf(g_tex_dir, sizeof(g_tex_dir), "%s", resolved);
    g_tex_ready = 1;
    return true;
}

const char* mb_textures_dir(void)
{
    return g_tex_ready ? g_tex_dir : NULL;
}

#ifdef _WIN32
static bool try_candidate(const char* root)
{
    char resolved[TEX_DIR_CAP];
    if (!resolve_block_dir(root, resolved, sizeof(resolved))) return false;
    snprintf(g_tex_dir, sizeof(g_tex_dir), "%s", resolved);
    g_tex_ready = 1;
    return true;
}

static bool exe_dir(char* out, size_t cap)
{
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return false;
    char* slash = strrchr(buf, '\\');
    if (!slash) return false;
    *slash = '\0';
    snprintf(out, cap, "%s", buf);
    return true;
}

/* Walk %APPDATA%\.minecraft\resourcepacks\* looking for an extracted pack. */
static void try_resourcepacks(void)
{
    const char* appdata = getenv("APPDATA");
    if (!appdata || !*appdata) return;

    char root[TEX_DIR_CAP];
    path_join(root, sizeof(root), appdata, ".minecraft/resourcepacks");
    char pattern[TEX_DIR_CAP + 8];
    path_join(pattern, sizeof(pattern), root, "*");

    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA(pattern, &data);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
        if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) continue;
        char pack[TEX_DIR_CAP];
        path_join(pack, sizeof(pack), root, data.cFileName);
        if (try_candidate(pack)) break;
    } while (FindNextFileA(h, &data));
    FindClose(h);
}
#else
static bool try_candidate(const char* root) { (void)root; return false; }
static bool exe_dir(char* out, size_t cap) { (void)out; (void)cap; return false; }
static void try_resourcepacks(void) {}
#endif

void mb_textures_autodetect(void)
{
    if (g_tex_detected) return;
    g_tex_detected = 1;

    const char* env = getenv("MB_TEXTURES");
    if (env && *env && try_candidate(env)) return;

    char dir[TEX_DIR_CAP];
    if (exe_dir(dir, sizeof(dir))) {
        char sub[TEX_DIR_CAP];
        path_join(sub, sizeof(sub), dir, "textures");
        if (try_candidate(sub)) return;
        path_join(sub, sizeof(sub), dir, "../textures");
        if (try_candidate(sub)) return;
    }
    if (try_candidate("textures")) return;
    if (try_candidate("../textures")) return;

    try_resourcepacks();
}

/* Vanilla filenames that do not match the block id one-to-one. */
static const char* texture_file_name(const char* shortId, char* buf, size_t cap)
{
    static const struct { const char* id; const char* file; } map[] = {
        { "grass_block", "grass_block_top" },
        { "water",       "water_still"     },
        { "glass_pane",  "glass_pane_top"  },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(map[i].id, shortId) == 0) return map[i].file;
    }
    snprintf(buf, cap, "%s", shortId);
    return buf;
}

#ifdef _WIN32
static bool utf8_to_wide(const char* src, wchar_t* out, size_t cap)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, src, -1, out, (int)cap);
    return n > 0;
}

static void release(IUnknown* p)
{
    if (p) p->lpVtbl->Release(p);
}

/* Decode a PNG with WIC and nearest-neighbour resample it to 16x16 RGB.
 * Animated strips (water_still.png is 16xN) use the first square frame. */
static bool wic_load_rgb(const char* path, unsigned char* rgb)
{
    static bool com_ready;
    if (!com_ready) {
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        com_ready = true;
    }

    IWICImagingFactory* factory = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IWICImagingFactory, (void**)&factory);
    if (FAILED(hr) || !factory) return false;

    wchar_t wpath[1024];
    bool ok = false;
    IWICBitmapDecoder* dec = NULL;
    IWICBitmapFrameDecode* frame = NULL;
    IWICFormatConverter* conv = NULL;

    if (!utf8_to_wide(path, wpath, sizeof(wpath) / sizeof(wpath[0]))) goto done;
    hr = factory->lpVtbl->CreateDecoderFromFilename(factory, wpath, NULL, GENERIC_READ,
                                                    WICDecodeMetadataCacheOnLoad, &dec);
    if (FAILED(hr) || !dec) goto done;
    hr = dec->lpVtbl->GetFrame(dec, 0, &frame);
    if (FAILED(hr) || !frame) goto done;

    UINT w = 0, h = 0;
    if (FAILED(frame->lpVtbl->GetSize(frame, &w, &h)) || w == 0 || h == 0) goto done;

    hr = factory->lpVtbl->CreateFormatConverter(factory, &conv);
    if (FAILED(hr) || !conv) goto done;
    hr = conv->lpVtbl->Initialize(conv, (IWICBitmapSource*)frame,
                                  &GUID_WICPixelFormat24bppBGR,
                                  WICBitmapDitherTypeNone, NULL, 0.0,
                                  WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) goto done;

    /* Use the first square frame of a vertical animation strip. */
    UINT sh = h > w ? w : h;
    size_t stride = (size_t)w * 3u;
    unsigned char* raw = (unsigned char*)malloc(stride * sh);
    if (!raw) goto done;
    hr = conv->lpVtbl->CopyPixels(conv, NULL, (UINT)stride, (UINT)(stride * sh), raw);
    if (FAILED(hr)) {
        free(raw);
        goto done;
    }

    for (int ty = 0; ty < TEX_SIZE; ty++) {
        UINT sy = (UINT)ty * sh / TEX_SIZE;
        if (sy >= sh) sy = sh - 1;
        for (int tx = 0; tx < TEX_SIZE; tx++) {
            UINT sx = (UINT)tx * w / TEX_SIZE;
            if (sx >= w) sx = w - 1;
            const unsigned char* src = raw + (size_t)sy * stride + (size_t)sx * 3;
            unsigned char* dst = rgb + ((size_t)ty * TEX_SIZE + tx) * 3;
            dst[0] = src[2];   /* BGR -> RGB */
            dst[1] = src[1];
            dst[2] = src[0];
        }
    }
    free(raw);
    ok = true;

done:
    release((IUnknown*)conv);
    release((IUnknown*)frame);
    release((IUnknown*)dec);
    release((IUnknown*)factory);
    return ok;
}
#else
static bool wic_load_rgb(const char* path, unsigned char* rgb)
{
    (void)path; (void)rgb;
    return false;
}
#endif

bool mb_textures_load(const char* shortId, unsigned char* rgb)
{
    if (!shortId || !rgb) return false;
    mb_textures_autodetect();
    if (!g_tex_ready) return false;

    char name[128];
    texture_file_name(shortId, name, sizeof(name));

    char path[TEX_DIR_CAP + 160];
    path_join(path, sizeof(path), g_tex_dir, name);
    size_t n = strlen(path);
    if (n + 5 < sizeof(path)) snprintf(path + n, sizeof(path) - n, ".png");

    return wic_load_rgb(path, rgb);
}
