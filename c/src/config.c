/*
 * config.c — the tiny per-user settings store behind MBLlmConfig.
 *
 * The desktop workbench and the terminal UI both edit the same five fields
 * (base URL, model, API key, temperature, JSON mode).  Keeping them in memory
 * meant every restart forgot the endpoint you had just typed in, so this module
 * persists them as a plain UTF-8 key=value file:
 *
 *     %APPDATA%\MinecraftBuilder\settings.ini
 *
 * A hand-written format instead of JSON is deliberate: it survives hand edits,
 * keeps the API key readable for the person who pasted it, and costs about
 * eighty lines instead of pulling the JSON parser into the settings path.  The
 * parser is deliberately forgiving — unknown keys, comments, blank lines and
 * CRLF endings are all ignored, so an older or newer file still works.
 */
#include "mb.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MB_MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define MB_MKDIR(path) mkdir((path), 0755)
#endif

#define CONFIG_LINE_MAX 1024

/* ========================================================================== */
/* path                                                                       */
/* ========================================================================== */

bool mb_config_path(char* out, size_t cap)
{
    if (!out || cap < 16) return false;

#ifdef _WIN32
    const char* base = getenv("APPDATA");
    if (!base || !*base) base = getenv("USERPROFILE");
    if (!base || !*base) return false;
    int n = snprintf(out, cap, "%s\\MinecraftBuilder\\settings.ini", base);
#else
    const char* base = getenv("XDG_CONFIG_HOME");
    if (base && *base) {
        int n = snprintf(out, cap, "%s/minecraft-builder/settings.ini", base);
        return n > 0 && (size_t)n < cap;
    }
    base = getenv("HOME");
    if (!base || !*base) return false;
    int n = snprintf(out, cap, "%s/.config/minecraft-builder/settings.ini", base);
#endif

    return n > 0 && (size_t)n < cap;
}

/* Create every missing parent directory of `path`.  Failure is not fatal: the
 * fopen() that follows reports the real problem. */
static void ensure_parent_dir(const char* path)
{
    char dir[512];
    int n = snprintf(dir, sizeof(dir), "%s", path);
    if (n <= 0 || (size_t)n >= sizeof(dir)) return;

    char* slash = strrchr(dir, '\\');
    char* fslash = strrchr(dir, '/');
    if (fslash && (!slash || fslash > slash)) slash = fslash;
    if (!slash || slash == dir) return;
    *slash = '\0';

    /* Walk the path and create each prefix.  The first component (drive letter
     * on Windows) is skipped because it always exists. */
    char* start = dir;
#ifdef _WIN32
    if (dir[1] == ':') start = dir + 2;
#endif
    for (char* p = start; *p; p++) {
        if (*p != '\\' && *p != '/') continue;
        char saved = *p;
        *p = '\0';
        (void)MB_MKDIR(dir);
        *p = saved;
    }
    (void)MB_MKDIR(dir);
}

/* ========================================================================== */
/* values                                                                     */
/* ========================================================================== */

static char* trim_in_place(char* s)
{
    if (!s) return s;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    size_t n = strlen(s);
    while (n > 0) {
        char c = s[n - 1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        s[--n] = '\0';
    }
    return s;
}

static void write_kv(FILE* f, const char* key, const char* value, bool* ok)
{
    if (!*ok) return;
    if (fprintf(f, "%s=", key) < 0) { *ok = false; return; }
    for (const char* p = value ? value : ""; *p; p++) {
        /* Newlines would split the value across records; a space is the least
         * surprising replacement.  URLs, model ids and keys never contain one. */
        char c = (*p == '\r' || *p == '\n') ? ' ' : *p;
        if (fputc(c, f) == EOF) { *ok = false; return; }
    }
    if (fputc('\n', f) == EOF) *ok = false;
}

/* ========================================================================== */
/* save                                                                       */
/* ========================================================================== */

bool mb_config_save_file(const char* path, const MBLlmConfig* cfg)
{
    if (!path || !*path || !cfg) return false;

    ensure_parent_dir(path);

    char temp[600];
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    FILE* f = fopen(temp, "wb");
    if (!f) return false;

    bool ok = true;
    write_kv(f, "base_url", cfg->baseUrl, &ok);
    write_kv(f, "model", cfg->model, &ok);
    write_kv(f, "api_key", cfg->apiKey, &ok);
    if (ok) {
        if (fprintf(f, "temperature=%.2f\n", cfg->temperature) < 0) ok = false;
        if (fprintf(f, "json_mode=%d\n", cfg->jsonMode ? 1 : 0) < 0) ok = false;
    }

    if (fclose(f) != 0) ok = false;
    if (!ok) {
        remove(temp);
        return false;
    }

    /* rename() does not replace an existing file on Windows. */
#ifdef _WIN32
    remove(path);
#endif
    if (rename(temp, path) != 0) {
        remove(temp);
        return false;
    }
    return true;
}

bool mb_config_save(const MBLlmConfig* cfg)
{
    char path[512];
    if (!mb_config_path(path, sizeof(path))) return false;
    return mb_config_save_file(path, cfg);
}

/* ========================================================================== */
/* load                                                                       */
/* ========================================================================== */

bool mb_config_load_file(const char* path, MBLlmConfig* cfg)
{
    if (!path || !*path || !cfg) return false;

    FILE* f = fopen(path, "rb");
    if (!f) return false;

    char line[CONFIG_LINE_MAX];
    int recognized = 0;

    while (fgets(line, sizeof(line), f)) {
        char* text = trim_in_place(line);
        if (!*text || *text == '#') continue;

        char* eq = strchr(text, '=');
        if (!eq) continue;
        *eq = '\0';

        char* key = trim_in_place(text);
        char* value = trim_in_place(eq + 1);

        if (strcmp(key, "base_url") == 0) {
            snprintf(cfg->baseUrl, sizeof(cfg->baseUrl), "%s", value);
            recognized++;
        } else if (strcmp(key, "model") == 0) {
            snprintf(cfg->model, sizeof(cfg->model), "%s", value);
            recognized++;
        } else if (strcmp(key, "api_key") == 0) {
            snprintf(cfg->apiKey, sizeof(cfg->apiKey), "%s", value);
            recognized++;
        } else if (strcmp(key, "temperature") == 0) {
            char* end = NULL;
            double d = strtod(value, &end);
            if (end != value) {
                if (d < 0.0) d = 0.0;
                if (d > 2.0) d = 2.0;
                cfg->temperature = d;
                recognized++;
            }
        } else if (strcmp(key, "json_mode") == 0) {
            cfg->jsonMode = (strcmp(value, "1") == 0
                             || strcmp(value, "true") == 0
                             || strcmp(value, "yes") == 0
                             || strcmp(value, "on") == 0);
            recognized++;
        }
    }

    fclose(f);
    return recognized > 0;
}

bool mb_config_load(MBLlmConfig* cfg)
{
    char path[512];
    if (!mb_config_path(path, sizeof(path))) return false;
    return mb_config_load_file(path, cfg);
}
