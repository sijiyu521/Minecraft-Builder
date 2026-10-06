/*
 * util.c — arena allocator, string buffers, UTF-8/UTF-16 conversion,
 *          deterministic RNG and small helpers.
 *
 * The RNG and the string hash are deliberately bit-for-bit copies of the
 * JavaScript implementation (mulberry32 + FNV-1a over UTF-16 code units) so a
 * prompt and a seed produce the exact same building in both editions.
 */
#include "mb.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* ========================================================================== */
/* arena                                                                      */
/* ========================================================================== */

#define MB_ARENA_BLOCK (64u * 1024u)
#define MB_ARENA_ALIGN 16u

struct ArenaBlock {
    ArenaBlock* next;
    size_t      used;
    size_t      cap;
    /* payload follows */
};

static size_t align_up(size_t n)
{
    return (n + (MB_ARENA_ALIGN - 1)) & ~(size_t)(MB_ARENA_ALIGN - 1);
}

void mb_arena_init(Arena* a)
{
    a->head = NULL;
}

void mb_arena_reset(Arena* a)
{
    ArenaBlock* blk = a->head;
    while (blk) {
        ArenaBlock* next = blk->next;
        free(blk);
        blk = next;
    }
    a->head = NULL;
}

/* Same as reset(), just spelled the way a caller tearing an arena down at the
   end of a scope expects to read it. */
void mb_arena_destroy(Arena* a)
{
    mb_arena_reset(a);
}

static ArenaBlock* arena_new_block(size_t payload)
{
    size_t cap = payload > MB_ARENA_BLOCK ? payload : MB_ARENA_BLOCK;
    ArenaBlock* blk = (ArenaBlock*)malloc(sizeof(ArenaBlock) + cap);
    if (!blk) {
        fputs("out of memory\n", stderr);
        exit(2);
    }
    blk->next = NULL;
    blk->used = 0;
    blk->cap = cap;
    return blk;
}

void* mb_arena_alloc(Arena* a, size_t n)
{
    n = align_up(n ? n : 1);
    if (!a->head || a->head->used + n > a->head->cap) {
        ArenaBlock* blk = arena_new_block(n);
        blk->next = a->head;
        a->head = blk;
    }
    {
        unsigned char* base = (unsigned char*)(a->head + 1);
        void* p = base + a->head->used;
        a->head->used += n;
        return p;
    }
}

void* mb_arena_calloc(Arena* a, size_t n)
{
    void* p = mb_arena_alloc(a, n);
    memset(p, 0, n);
    return p;
}

char* mb_arena_strndup(Arena* a, const char* s, size_t n)
{
    char* p = (char*)mb_arena_alloc(a, n + 1);
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

char* mb_arena_strdup(Arena* a, const char* s)
{
    if (!s) return NULL;
    return mb_arena_strndup(a, s, strlen(s));
}

char* mb_arena_printf(Arena* a, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) {
        va_end(ap2);
        return mb_arena_strdup(a, "");
    }
    char* p = (char*)mb_arena_alloc(a, (size_t)need + 1);
    vsnprintf(p, (size_t)need + 1, fmt, ap2);
    va_end(ap2);
    return p;
}

/* ========================================================================== */
/* string buffer                                                              */
/* ========================================================================== */

void mb_sb_init(SBuf* b, Arena* a)
{
    b->arena = a;
    b->cap = 1024;
    b->len = 0;
    b->data = (char*)mb_arena_alloc(a, b->cap);
    b->data[0] = '\0';
}

static void sb_grow(SBuf* b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap) return;
    size_t cap = b->cap;
    while (cap < b->len + extra + 1) cap *= 2;
    char* next = (char*)mb_arena_alloc(b->arena, cap);
    memcpy(next, b->data, b->len);
    next[b->len] = '\0';
    b->data = next;
    b->cap = cap;
}

void mb_sb_putc(SBuf* b, char c)
{
    sb_grow(b, 1);
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

void mb_sb_write(SBuf* b, const char* s, size_t n)
{
    if (!n) return;
    sb_grow(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void mb_sb_puts(SBuf* b, const char* s)
{
    if (!s) return;
    mb_sb_write(b, s, strlen(s));
}

void mb_sb_printf(SBuf* b, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need > 0) {
        sb_grow(b, (size_t)need);
        vsnprintf(b->data + b->len, (size_t)need + 1, fmt, ap2);
        b->len += (size_t)need;
    }
    va_end(ap2);
}

char* mb_sb_done(SBuf* b)
{
    return mb_arena_strndup(b->arena, b->data, b->len);
}

/* ========================================================================== */
/* UTF-8 <-> UTF-16                                                           */
/* ========================================================================== */

/* Decode one UTF-8 sequence. Returns the number of bytes consumed and stores a
 * code point. Malformed input yields U+FFFD and advances one byte. */
static size_t utf8_next(const char* s, uint32_t* out)
{
    const unsigned char* p = (const unsigned char*)s;
    unsigned char c = p[0];

    if (c < 0x80) {
        *out = c;
        return 1;
    }
    if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *out = (uint32_t)(((c & 0x1Fu) << 6) | (p[1] & 0x3Fu));
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *out = (uint32_t)(((c & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu));
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        *out = (uint32_t)(((c & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) |
                          ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu));
        return 4;
    }
    *out = 0xFFFD;
    return 1;
}

U16* mb_u16_from_utf8(Arena* a, const char* s, size_t* outLen)
{
    size_t bytes = s ? strlen(s) : 0;
    /* Worst case: every byte becomes one BMP unit, or 2 units per 4 bytes. */
    U16* buf = (U16*)mb_arena_alloc(a, (bytes * 2 + 1) * sizeof(U16));
    size_t n = 0;
    size_t i = 0;

    while (i < bytes) {
        uint32_t cp = 0;
        size_t used = utf8_next(s + i, &cp);
        i += used;
        if (cp >= 0x10000u) {
            uint32_t v = cp - 0x10000u;
            buf[n++] = (U16)(0xD800u + (v >> 10));
            buf[n++] = (U16)(0xDC00u + (v & 0x3FFu));
        } else {
            buf[n++] = (U16)cp;
        }
    }
    buf[n] = 0;
    if (outLen) *outLen = n;
    return buf;
}

size_t mb_u16_len(const char* utf8)
{
    if (!utf8) return 0;
    size_t bytes = strlen(utf8);
    size_t n = 0;
    size_t i = 0;
    while (i < bytes) {
        uint32_t cp = 0;
        size_t used = utf8_next(utf8 + i, &cp);
        i += used;
        n += (cp >= 0x10000u) ? 2u : 1u;
    }
    return n;
}

size_t mb_u16_len_buf(const U16* s)
{
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static size_t utf8_put(char* out, uint32_t cp)
{
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

char* mb_utf8_from_u16(Arena* a, const U16* s, size_t len)
{
    char* out = (char*)mb_arena_alloc(a, len * 4 + 1);
    size_t n = 0;
    size_t i = 0;
    while (i < len) {
        uint32_t cp = s[i++];
        if (cp >= 0xD800u && cp <= 0xDBFFu && i < len && s[i] >= 0xDC00u && s[i] <= 0xDFFFu) {
            cp = 0x10000u + ((cp - 0xD800u) << 10) + (s[i] - 0xDC00u);
            i++;
        }
        n += utf8_put(out + n, cp);
    }
    out[n] = '\0';
    return out;
}

/* ========================================================================== */
/* helpers                                                                    */
/* ========================================================================== */

uint32_t mb_hash_string(const char* utf8)
{
    uint32_t h = 2166136261u;
    if (!utf8) return h;

    size_t bytes = strlen(utf8);
    size_t i = 0;
    while (i < bytes) {
        uint32_t cp = 0;
        size_t used = utf8_next(utf8 + i, &cp);
        i += used;
        if (cp >= 0x10000u) {
            uint32_t v = cp - 0x10000u;
            uint32_t hi = 0xD800u + (v >> 10);
            uint32_t lo = 0xDC00u + (v & 0x3FFu);
            h ^= hi;
            h *= 16777619u;
            h ^= lo;
            h *= 16777619u;
        } else {
            h ^= cp;
            h *= 16777619u;
        }
    }
    return h;
}

void mb_rng_seed(uint32_t* state, uint32_t seed)
{
    *state = seed;
}

double mb_rng_next(uint32_t* state)
{
    uint32_t a = *state + 0x6d2b79f5u;
    uint32_t t;
    *state = a;
    t = a;
    /* Math.imul == low 32 bits of the product. */
    t = (t ^ (t >> 15)) * (t | 1u);
    t ^= t + ((t ^ (t >> 7)) * (t | 61u));
    return (double)(t ^ (t >> 14)) / 4294967296.0;
}

int mb_rand_int(uint32_t* state, int min, int max)
{
    return min + (int)(mb_rng_next(state) * (double)(max - min + 1));
}

/*
 * Math.round() rounds halves towards +Infinity (Math.round(-2.5) === -2), which
 * is NOT what C round() does (that goes away from zero). floor(v + 0.5) matches
 * the JavaScript behaviour exactly, and getting this wrong would silently shift
 * every generated building by a block.
 */
int mb_js_round(double v)
{
    double r;
    if (v != v) return 0;                      /* NaN */
    r = floor(v + 0.5);
    if (r > 2147483647.0) return 2147483647;
    if (r < -2147483648.0) return -2147483647 - 1;
    return (int)r;
}

int mb_clamp_int(int v, int min, int max)
{
    if (v < min) return min;
    if (v > max) return max;
    return v;
}

char* mb_trim_dup(Arena* a, const char* s)
{
    if (!s) return mb_arena_strdup(a, "");
    const char* start = s;
    while (*start && (unsigned char)*start <= ' ') start++;
    const char* end = s + strlen(s);
    while (end > start && (unsigned char)end[-1] <= ' ') end--;
    return mb_arena_strndup(a, start, (size_t)(end - start));
}

bool mb_ieq(const char* a, const char* b)
{
    if (a == b) return true;
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

char* mb_lower_dup(Arena* a, const char* s)
{
    char* out = mb_arena_strdup(a, s ? s : "");
    for (char* p = out; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
    }
    return out;
}

bool mb_u16_is_space(U16 c)
{
    switch (c) {
    case 0x0009: case 0x000A: case 0x000B: case 0x000C: case 0x000D:
    case 0x0020: case 0x00A0: case 0x1680: case 0x2028: case 0x2029:
    case 0x202F: case 0x205F: case 0x3000: case 0xFEFF:
        return true;
    default:
        return c >= 0x2000 && c <= 0x200A;
    }
}

static int lower_ascii(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
    return c;
}

const char* mb_strstr_ci(const char* haystack, const char* needle)
{
    if (!haystack || !needle) return NULL;
    size_t nn = strlen(needle);
    if (nn == 0) return haystack;
    for (const char* p = haystack; *p; p++) {
        size_t i = 0;
        while (i < nn && p[i] &&
               lower_ascii((unsigned char)p[i]) == lower_ascii((unsigned char)needle[i])) {
            i++;
        }
        if (i == nn) return p;
    }
    return NULL;
}

bool mb_contains_ci(const char* haystack, const char* needle)
{
    return mb_strstr_ci(haystack, needle) != NULL;
}

int mb_strncmp_ci(const char* a, const char* b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int ca = lower_ascii((unsigned char)a[i]);
        int cb = lower_ascii((unsigned char)b[i]);
        if (ca != cb) return ca - cb;
        if (ca == 0) return 0;
    }
    return 0;
}

double mb_now_ms(void)
{
#ifdef _WIN32
    return (double)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
#endif
}
