/*
 * json.c — a small, strict JSON reader/writer.
 *
 * Why hand rolled? Because the LLM validation layer must be able to inspect a
 * model's answer, report precise problems ("结构结构 is not a string") and then
 * fall back to safe defaults. Having the tree in memory, rather than validating
 * field by field while streaming, makes that trivial — and it keeps the whole
 * project dependency free.
 *
 * The parser accepts everything a chat completion endpoint realistically
 * returns, including \uXXXX escapes and surrogate pairs.
 */
#include "mb.h"

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define MB_JSON_MAX_DEPTH 64

typedef struct {
    Arena*      a;
    const char* p;
    const char* end;
    int         depth;
    bool        failed;
} JParser;

static MBJson* jnew(JParser* P, MBJsonType t)
{
    MBJson* v = (MBJson*)mb_arena_calloc(P->a, sizeof(MBJson));
    v->type = t;
    return v;
}

static void skip_ws(JParser* P)
{
    while (P->p < P->end) {
        char c = *P->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { P->p++; continue; }
        break;
    }
}

static MBJson* parse_value(JParser* P);

static void utf8_encode(char** out, uint32_t cp)
{
    char* o = *out;
    if (cp < 0x80u) {
        *o++ = (char)cp;
    } else if (cp < 0x800u) {
        *o++ = (char)(0xC0u | (cp >> 6));
        *o++ = (char)(0x80u | (cp & 0x3Fu));
    } else if (cp < 0x10000u) {
        *o++ = (char)(0xE0u | (cp >> 12));
        *o++ = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        *o++ = (char)(0x80u | (cp & 0x3Fu));
    } else {
        *o++ = (char)(0xF0u | (cp >> 18));
        *o++ = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        *o++ = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        *o++ = (char)(0x80u | (cp & 0x3Fu));
    }
    *out = o;
}

static int hex4(const char* s)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

/* Parse a string literal, returning an arena copy. NULL on failure. */
static char* parse_string_raw(JParser* P)
{
    if (P->p >= P->end || *P->p != '"') { P->failed = true; return NULL; }
    P->p++;

    SBuf b;
    mb_sb_init(&b, P->a);

    while (P->p < P->end) {
        unsigned char c = (unsigned char)*P->p;
        if (c == '"') {
            P->p++;
            char* out = (char*)mb_arena_alloc(P->a, b.len + 1);
            memcpy(out, b.data, b.len);
            out[b.len] = '\0';
            return out;
        }
        if (c == '\\') {
            P->p++;
            if (P->p >= P->end) break;
            char e = *P->p++;
            switch (e) {
            case '"':  mb_sb_putc(&b, '"');  break;
            case '\\': mb_sb_putc(&b, '\\'); break;
            case '/':  mb_sb_putc(&b, '/');  break;
            case 'b':  mb_sb_putc(&b, '\b'); break;
            case 'f':  mb_sb_putc(&b, '\f'); break;
            case 'n':  mb_sb_putc(&b, '\n'); break;
            case 'r':  mb_sb_putc(&b, '\r'); break;
            case 't':  mb_sb_putc(&b, '\t'); break;
            case 'u': {
                if (P->end - P->p < 4) { P->failed = true; return NULL; }
                int cp = hex4(P->p);
                if (cp < 0) { P->failed = true; return NULL; }
                P->p += 4;
                /* Combine a surrogate pair into one code point. */
                if (cp >= 0xD800 && cp <= 0xDBFF && P->end - P->p >= 6 &&
                    P->p[0] == '\\' && P->p[1] == 'u') {
                    int lo = hex4(P->p + 2);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        P->p += 6;
                    }
                }
                char* w = b.data + b.len;
                utf8_encode(&w, (uint32_t)cp);
                b.len = (size_t)(w - b.data);
                b.data[b.len] = '\0';
                break;
            }
            default:
                /* Be forgiving about unknown escapes instead of dying. */
                mb_sb_putc(&b, e);
                break;
            }
            continue;
        }
        if (c < 0x20) { P->failed = true; return NULL; }
        mb_sb_putc(&b, (char)c);
        P->p++;
    }

    P->failed = true;
    return NULL;
}

static MBJson* parse_string(JParser* P)
{
    char* s = parse_string_raw(P);
    if (!s) return NULL;
    MBJson* v = jnew(P, MB_JSON_STR);
    v->u.str.ptr = s;
    v->u.str.len = strlen(s);
    return v;
}

static MBJson* parse_number(JParser* P)
{
    const char* start = P->p;
    char buf[64];
    size_t n = 0;

    if (P->p < P->end && (*P->p == '-' || *P->p == '+')) {
        if (n < sizeof(buf) - 1) buf[n++] = *P->p;
        P->p++;
    }
    while (P->p < P->end &&
           ((*P->p >= '0' && *P->p <= '9') || *P->p == '.' ||
            *P->p == 'e' || *P->p == 'E' || *P->p == '+' || *P->p == '-')) {
        if (n < sizeof(buf) - 1) buf[n++] = *P->p;
        P->p++;
    }
    buf[n] = '\0';
    if (P->p == start) { P->failed = true; return NULL; }

    MBJson* v = jnew(P, MB_JSON_NUM);
    v->u.num = strtod(buf, NULL);
    return v;
}

static MBJson* parse_array(JParser* P)
{
    P->p++;  /* '[' */
    MBJson* v = jnew(P, MB_JSON_ARR);

    size_t cap = 8;
    v->u.arr.items = (MBJson**)mb_arena_alloc(P->a, cap * sizeof(MBJson*));
    v->u.arr.count = 0;

    skip_ws(P);
    if (P->p < P->end && *P->p == ']') { P->p++; return v; }

    for (;;) {
        MBJson* item = parse_value(P);
        if (!item) return NULL;
        if (v->u.arr.count == cap) {
            MBJson** bigger = (MBJson**)mb_arena_alloc(P->a, cap * 2 * sizeof(MBJson*));
            memcpy(bigger, v->u.arr.items, cap * sizeof(MBJson*));
            v->u.arr.items = bigger;
            cap *= 2;
        }
        v->u.arr.items[v->u.arr.count++] = item;

        skip_ws(P);
        if (P->p < P->end && *P->p == ',') { P->p++; skip_ws(P); continue; }
        if (P->p < P->end && *P->p == ']') { P->p++; return v; }
        P->failed = true;
        return NULL;
    }
}

static MBJson* parse_object(JParser* P)
{
    P->p++;  /* '{' */
    MBJson* v = jnew(P, MB_JSON_OBJ);

    size_t cap = 8;
    v->u.obj.keys = (char**)mb_arena_alloc(P->a, cap * sizeof(char*));
    v->u.obj.vals = (MBJson**)mb_arena_alloc(P->a, cap * sizeof(MBJson*));
    v->u.obj.count = 0;

    skip_ws(P);
    if (P->p < P->end && *P->p == '}') { P->p++; return v; }

    for (;;) {
        skip_ws(P);
        char* key = parse_string_raw(P);
        if (!key) return NULL;
        skip_ws(P);
        if (P->p >= P->end || *P->p != ':') { P->failed = true; return NULL; }
        P->p++;
        MBJson* val = parse_value(P);
        if (!val) return NULL;

        if (v->u.obj.count == cap) {
            char** k2 = (char**)mb_arena_alloc(P->a, cap * 2 * sizeof(char*));
            MBJson** v2 = (MBJson**)mb_arena_alloc(P->a, cap * 2 * sizeof(MBJson*));
            memcpy(k2, v->u.obj.keys, cap * sizeof(char*));
            memcpy(v2, v->u.obj.vals, cap * sizeof(MBJson*));
            v->u.obj.keys = k2;
            v->u.obj.vals = v2;
            cap *= 2;
        }
        v->u.obj.keys[v->u.obj.count] = key;
        v->u.obj.vals[v->u.obj.count] = val;
        v->u.obj.count++;

        skip_ws(P);
        if (P->p < P->end && *P->p == ',') { P->p++; continue; }
        if (P->p < P->end && *P->p == '}') { P->p++; return v; }
        P->failed = true;
        return NULL;
    }
}

static MBJson* parse_value(JParser* P)
{
    skip_ws(P);
    if (P->p >= P->end) { P->failed = true; return NULL; }
    if (++P->depth > MB_JSON_MAX_DEPTH) { P->failed = true; return NULL; }

    char c = *P->p;
    MBJson* out = NULL;

    if (c == '{') out = parse_object(P);
    else if (c == '[') out = parse_array(P);
    else if (c == '"') out = parse_string(P);
    else if (c == 't' && P->end - P->p >= 4 && memcmp(P->p, "true", 4) == 0) {
        P->p += 4;
        out = jnew(P, MB_JSON_BOOL);
        out->u.boolean = true;
    } else if (c == 'f' && P->end - P->p >= 5 && memcmp(P->p, "false", 5) == 0) {
        P->p += 5;
        out = jnew(P, MB_JSON_BOOL);
        out->u.boolean = false;
    } else if (c == 'n' && P->end - P->p >= 4 && memcmp(P->p, "null", 4) == 0) {
        P->p += 4;
        out = jnew(P, MB_JSON_NULL);
    } else if (c == '-' || c == '+' || (c >= '0' && c <= '9')) {
        out = parse_number(P);
    } else {
        P->failed = true;
    }

    P->depth--;
    return out;
}

MBJson* mb_json_parse(Arena* a, const char* text, size_t len)
{
    if (!text) return NULL;
    JParser P;
    P.a = a;
    P.p = text;
    P.end = text + len;
    P.depth = 0;
    P.failed = false;

    MBJson* v = parse_value(&P);
    if (!v || P.failed) return NULL;
    skip_ws(&P);
    /* Trailing garbage means it was not JSON after all. */
    if (P.p != P.end) return NULL;
    return v;
}

MBJson* mb_json_get(const MBJson* obj, const char* key)
{
    if (!obj || obj->type != MB_JSON_OBJ || !key) return NULL;
    for (size_t i = 0; i < obj->u.obj.count; i++) {
        if (strcmp(obj->u.obj.keys[i], key) == 0) return obj->u.obj.vals[i];
    }
    return NULL;
}

MBJson* mb_json_at(const MBJson* arr, size_t index)
{
    if (!arr || arr->type != MB_JSON_ARR) return NULL;
    if (index >= arr->u.arr.count) return NULL;
    return arr->u.arr.items[index];
}

const char* mb_json_type_name(MBJsonType t)
{
    switch (t) {
    case MB_JSON_NULL: return "null";
    case MB_JSON_BOOL: return "boolean";
    case MB_JSON_NUM:  return "number";
    case MB_JSON_STR:  return "string";
    case MB_JSON_ARR:  return "array";
    case MB_JSON_OBJ:  return "object";
    }
    return "unknown";
}

/* Shortest decimal representation that round-trips, like JavaScript's
 * Number.prototype.toString(). */
static void js_number(char* buf, size_t cap, double v)
{
    if (v != v)            { snprintf(buf, cap, "NaN"); return; }
    if (v == (double)INFINITY)  { snprintf(buf, cap, "Infinity"); return; }
    if (v == -(double)INFINITY) { snprintf(buf, cap, "-Infinity"); return; }
    if (v == 0.0)          { snprintf(buf, cap, "0"); return; }

    if (v == floor(v) && fabs(v) < 1e21) {
        snprintf(buf, cap, "%.0f", v);
        return;
    }
    for (int prec = 1; prec <= 17; prec++) {
        snprintf(buf, cap, "%.*g", prec, v);
        if (strtod(buf, NULL) == v) return;
    }
}

char* mb_json_to_js_string(Arena* a, const MBJson* v)
{
    if (!v) return mb_arena_strdup(a, "undefined");
    switch (v->type) {
    case MB_JSON_NULL: return mb_arena_strdup(a, "null");
    case MB_JSON_BOOL: return mb_arena_strdup(a, v->u.boolean ? "true" : "false");
    case MB_JSON_NUM: {
        char buf[64];
        js_number(buf, sizeof(buf), v->u.num);
        return mb_arena_strdup(a, buf);
    }
    case MB_JSON_STR:
        return mb_arena_strdup(a, v->u.str.ptr);
    case MB_JSON_ARR: {
        SBuf b;
        mb_sb_init(&b, a);
        for (size_t i = 0; i < v->u.arr.count; i++) {
            if (i) mb_sb_putc(&b, ',');
            mb_sb_puts(&b, mb_json_to_js_string(a, v->u.arr.items[i]));
        }
        return mb_sb_done(&b);
    }
    case MB_JSON_OBJ:
    default:
        return mb_arena_strdup(a, "[object Object]");
    }
}

void mb_json_escape(SBuf* b, const char* s)
{
    mb_sb_putc(b, '"');
    for (const unsigned char* p = (const unsigned char*)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '"':  mb_sb_puts(b, "\\\""); break;
        case '\\': mb_sb_puts(b, "\\\\"); break;
        case '\b': mb_sb_puts(b, "\\b"); break;
        case '\f': mb_sb_puts(b, "\\f"); break;
        case '\n': mb_sb_puts(b, "\\n"); break;
        case '\r': mb_sb_puts(b, "\\r"); break;
        case '\t': mb_sb_puts(b, "\\t"); break;
        default:
            if (*p < 0x20) mb_sb_printf(b, "\\u%04x", (unsigned)*p);
            else mb_sb_putc(b, (char)*p);
            break;
        }
    }
    mb_sb_putc(b, '"');
}

static void stringify_into(SBuf* b, const MBJson* v, bool pretty, int indent)
{
    if (!v) { mb_sb_puts(b, "null"); return; }

    switch (v->type) {
    case MB_JSON_NULL: mb_sb_puts(b, "null"); break;
    case MB_JSON_BOOL: mb_sb_puts(b, v->u.boolean ? "true" : "false"); break;
    case MB_JSON_NUM: {
        char buf[64];
        js_number(buf, sizeof(buf), v->u.num);
        mb_sb_puts(b, buf);
        break;
    }
    case MB_JSON_STR: mb_json_escape(b, v->u.str.ptr); break;

    case MB_JSON_ARR: {
        if (v->u.arr.count == 0) { mb_sb_puts(b, "[]"); break; }
        mb_sb_putc(b, '[');
        for (size_t i = 0; i < v->u.arr.count; i++) {
            if (i) mb_sb_putc(b, ',');
            if (pretty) { mb_sb_putc(b, '\n'); for (int k = 0; k <= indent; k++) mb_sb_puts(b, "  "); }
            stringify_into(b, v->u.arr.items[i], pretty, indent + 1);
        }
        if (pretty) { mb_sb_putc(b, '\n'); for (int k = 0; k < indent; k++) mb_sb_puts(b, "  "); }
        mb_sb_putc(b, ']');
        break;
    }

    case MB_JSON_OBJ: {
        if (v->u.obj.count == 0) { mb_sb_puts(b, "{}"); break; }
        mb_sb_putc(b, '{');
        for (size_t i = 0; i < v->u.obj.count; i++) {
            if (i) mb_sb_putc(b, ',');
            if (pretty) { mb_sb_putc(b, '\n'); for (int k = 0; k <= indent; k++) mb_sb_puts(b, "  "); }
            mb_json_escape(b, v->u.obj.keys[i]);
            mb_sb_puts(b, pretty ? ": " : ":");
            stringify_into(b, v->u.obj.vals[i], pretty, indent + 1);
        }
        if (pretty) { mb_sb_putc(b, '\n'); for (int k = 0; k < indent; k++) mb_sb_puts(b, "  "); }
        mb_sb_putc(b, '}');
        break;
    }
    }
}

char* mb_json_stringify(Arena* a, const MBJson* v, bool pretty)
{
    SBuf b;
    mb_sb_init(&b, a);
    stringify_into(&b, v, pretty, 0);
    return mb_sb_done(&b);
}
