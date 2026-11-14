/*
 * launcher.c — Minecraft version discovery, installation and offline launch.
 *
 * This is intentionally a self-contained module: it reads the same .minecraft
 * layout the official launcher uses, pulls version metadata from Mojang, and
 * starts javaw. It never bundles Mojang binaries; everything lands in the
 * user's own game directory.
 *
 * Supported today: vanilla versions (old and new argument formats), libraries,
 * natives and assets. Mod loaders that use inheritsFrom are rejected with a
 * clear message instead of launching something half broken.
 */
#include "launcher.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <bcrypt.h>
#endif

#define VERSION_MANIFEST_URL "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"
#define RESOURCES_BASE       "https://resources.download.minecraft.net/"
#define LIBRARIES_BASE       "https://libraries.minecraft.net/"

typedef struct {
    char      path[1024];
    char      url[1024];
    long long size;
} MBFileRef;

typedef struct {
    char      name[192];
    char      nativesClass[64];   /* e.g. natives-windows, empty if none */
    MBFileRef artifact;
    MBFileRef natives;
} MBLib;

typedef struct {
    char      id[64];
    char      mainClass[160];
    char      assetIndexId[64];
    char      assetIndexUrl[1024];
    char      type[32];
    int       javaMajor;
    MBFileRef client;
    MBLib     libs[320];
    int       libCount;
    char      gameArgs[8192];     /* already rule-filtered, placeholders kept */
    char      jvmArgs[4096];
} MBVersionMeta;

/* ========================================================================== */
/* small helpers                                                              */
/* ========================================================================== */

static void seterr(char* err, size_t cap, const char* fmt, ...)
{
    if (!err || cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

static bool file_size_of(const char* path, long long* size)
{
    if (size) *size = -1;
#ifdef _WIN32
    wchar_t w[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024)) return false;
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(w, GetFileExInfoStandard, &d)) return false;
    if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    if (size) *size = ((long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    return true;
#else
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) == 0 && size) *size = ftell(f);
    fclose(f);
    return true;
#endif
}

static bool is_dir(const char* path)
{
#ifdef _WIN32
    wchar_t w[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024)) return false;
    DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    (void)path;
    return false;
#endif
}

static bool mkdirs(const char* path)
{
    if (!path || !*path) return false;
    if (is_dir(path)) return true;
#ifdef _WIN32
    wchar_t w[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024)) return false;
    int rc = SHCreateDirectoryExW(NULL, w, NULL);
    return rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS || rc == ERROR_FILE_EXISTS;
#else
    return false;
#endif
}

static char* read_text(Arena* a, const char* path, size_t* outLen)
{
#ifdef _WIN32
    wchar_t w[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024)) return NULL;
    FILE* f = _wfopen(w, L"rb");
#else
    FILE* f = fopen(path, "rb");
#endif
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char* buf = (char*)mb_arena_alloc(a, (size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (outLen) *outLen = got;
    return buf;
}

/* paths use backslashes on Windows; the internet uses forward slashes */
static void replace_slashes(char* s)
{
    for (; *s; s++) if (*s == '/') *s = '\\';
}

static bool need_download(const char* path, long long size)
{
    long long have = 0;
    if (!file_size_of(path, &have)) return true;
    if (size > 0) return have != size;
    return false;
}

#ifdef _WIN32
static wchar_t* widen(const char* s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t* w = (wchar_t*)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static bool run_hidden(const wchar_t* cmdline, const wchar_t* cwd)
{
    wchar_t* mutableCmd = _wcsdup(cmdline);
    if (!mutableCmd) return false;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    BOOL ok = CreateProcessW(NULL, mutableCmd, NULL, NULL, FALSE,
                             CREATE_NO_WINDOW, NULL, cwd, &si, &pi);
    free(mutableCmd);
    if (!ok) return false;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}
#endif

/* ========================================================================== */
/* JSON helpers                                                               */
/* ========================================================================== */

static const char* jstr(const MBJson* obj, const char* key, const char* fallback)
{
    const MBJson* v = mb_json_get(obj, key);
    return (v && v->type == MB_JSON_STR && v->u.str.ptr) ? v->u.str.ptr : fallback;
}

static long long jnum(const MBJson* obj, const char* key, long long fallback)
{
    const MBJson* v = mb_json_get(obj, key);
    return (v && v->type == MB_JSON_NUM) ? (long long)v->u.num : fallback;
}

static void copy_str(char* dst, size_t cap, const char* src)
{
    if (!src) src = "";
    snprintf(dst, cap, "%s", src);
}

/* Rules are action/os/features entries; we only run on Windows and have no
 * optional features enabled, so this covers vanilla metadata. */
static bool rules_allow(const MBJson* rules)
{
    if (!rules) return true;
    if (rules->type != MB_JSON_ARR) return true;

    bool allow = false;
    for (size_t i = 0; i < rules->u.arr.count; i++) {
        const MBJson* rule = mb_json_at(rules, i);
        if (!rule || rule->type != MB_JSON_OBJ) continue;
        const MBJson* os = mb_json_get(rule, "os");
        if (os) {
            const char* name = jstr(os, "name", "");
            if (strcmp(name, "windows") != 0) continue;
        }
        const MBJson* feat = mb_json_get(rule, "features");
        if (feat && feat->type == MB_JSON_OBJ) {
            /* No optional features are enabled in this launcher. */
            continue;
        }
        const char* action = jstr(rule, "action", "allow");
        allow = strcmp(action, "disallow") != 0;
    }
    return allow;
}

static void append(char* buf, size_t cap, const char* text)
{
    size_t n = strlen(buf);
    if (n < cap - 1) snprintf(buf + n, cap - n, "%s", text);
}

/* Collect argument strings from the new `arguments.game` / `arguments.jvm`
 * arrays, applying rules. */
static void collect_args(const MBJson* arr, char* out, size_t cap)
{
    if (!arr || arr->type != MB_JSON_ARR) return;
    for (size_t i = 0; i < arr->u.arr.count; i++) {
        const MBJson* e = mb_json_at(arr, i);
        if (!e) continue;
        if (e->type == MB_JSON_STR && e->u.str.ptr) {
            append(out, cap, e->u.str.ptr);
            append(out, cap, " ");
        } else if (e->type == MB_JSON_OBJ) {
            if (!rules_allow(mb_json_get(e, "rules"))) continue;
            const MBJson* value = mb_json_get(e, "value");
            if (!value) continue;
            if (value->type == MB_JSON_STR && value->u.str.ptr) {
                append(out, cap, value->u.str.ptr);
                append(out, cap, " ");
            } else if (value->type == MB_JSON_ARR) {
                for (size_t k = 0; k < value->u.arr.count; k++) {
                    const MBJson* s = mb_json_at(value, k);
                    if (s && s->type == MB_JSON_STR && s->u.str.ptr) {
                        append(out, cap, s->u.str.ptr);
                        append(out, cap, " ");
                    }
                }
            }
        }
    }
}

/* group:artifact:version -> group/path/artifact/version/artifact-version.jar */
static void maven_path(const char* name, char* out, size_t cap)
{
    char tmp[192];
    copy_str(tmp, sizeof(tmp), name);
    char* parts[3] = { NULL, NULL, NULL };
    int n = 0;
    char* p = tmp;
    while (p && n < 3) {
        parts[n++] = p;
        char* colon = strchr(p, ':');
        if (!colon) break;
        *colon = '\0';
        p = colon + 1;
    }
    if (n < 3) { out[0] = '\0'; return; }
    for (char* q = parts[0]; *q; q++) if (*q == '.') *q = '/';
    snprintf(out, cap, "%s/%s/%s/%s-%s.jar", parts[0], parts[1], parts[2], parts[1], parts[2]);
    replace_slashes(out);
}

/* ========================================================================== */
/* version metadata                                                           */
/* ========================================================================== */

static void parse_version_meta(const MBJson* root, MBVersionMeta* m)
{
    memset(m, 0, sizeof(*m));
    copy_str(m->id, sizeof(m->id), jstr(root, "id", ""));
    copy_str(m->mainClass, sizeof(m->mainClass), jstr(root, "mainClass", ""));
    copy_str(m->type, sizeof(m->type), jstr(root, "type", "release"));

    const MBJson* java = mb_json_get(root, "javaVersion");
    m->javaMajor = (int)jnum(java, "majorVersion", 0);

    const MBJson* assets = mb_json_get(root, "assetIndex");
    if (assets) {
        copy_str(m->assetIndexId, sizeof(m->assetIndexId), jstr(assets, "id", ""));
        copy_str(m->assetIndexUrl, sizeof(m->assetIndexUrl), jstr(assets, "url", ""));
    }

    const MBJson* downloads = mb_json_get(root, "downloads");
    const MBJson* client = downloads ? mb_json_get(downloads, "client") : NULL;
    if (client) {
        copy_str(m->client.path, sizeof(m->client.path), jstr(client, "path", ""));
        copy_str(m->client.url, sizeof(m->client.url), jstr(client, "url", ""));
        m->client.size = jnum(client, "size", 0);
    }

    const MBJson* args = mb_json_get(root, "arguments");
    if (args) {
        collect_args(mb_json_get(args, "game"), m->gameArgs, sizeof(m->gameArgs));
        collect_args(mb_json_get(args, "jvm"), m->jvmArgs, sizeof(m->jvmArgs));
    } else {
        copy_str(m->gameArgs, sizeof(m->gameArgs), jstr(root, "minecraftArguments", ""));
    }

    const MBJson* libs = mb_json_get(root, "libraries");
    if (libs && libs->type == MB_JSON_ARR) {
        for (size_t i = 0; i < libs->u.arr.count && m->libCount < 320; i++) {
            const MBJson* lib = mb_json_at(libs, i);
            if (!lib || lib->type != MB_JSON_OBJ) continue;
            if (!rules_allow(mb_json_get(lib, "rules"))) continue;

            MBLib* out = &m->libs[m->libCount];
            const char* name = jstr(lib, "name", "");
            if (!*name) continue;
            copy_str(out->name, sizeof(out->name), name);

            const MBJson* dls = mb_json_get(lib, "downloads");
            const MBJson* art = dls ? mb_json_get(dls, "artifact") : NULL;
            if (art) {
                copy_str(out->artifact.path, sizeof(out->artifact.path), jstr(art, "path", ""));
                copy_str(out->artifact.url, sizeof(out->artifact.url), jstr(art, "url", ""));
                out->artifact.size = jnum(art, "size", 0);
            } else {
                maven_path(name, out->artifact.path, sizeof(out->artifact.path));
                if (out->artifact.path[0]) {
                    snprintf(out->artifact.url, sizeof(out->artifact.url), "%s%s",
                             LIBRARIES_BASE, out->artifact.path);
                    for (char* q = out->artifact.url; *q; q++) if (*q == '\\') *q = '/';
                }
            }
            replace_slashes(out->artifact.path);

            const MBJson* natives = mb_json_get(lib, "natives");
            const char* cls = natives ? jstr(natives, "windows", "") : "";
            if (*cls) {
                copy_str(out->nativesClass, sizeof(out->nativesClass), cls);
                const MBJson* classifiers = dls ? mb_json_get(dls, "classifiers") : NULL;
                const MBJson* nat = classifiers ? mb_json_get(classifiers, cls) : NULL;
                if (nat) {
                    copy_str(out->natives.path, sizeof(out->natives.path), jstr(nat, "path", ""));
                    copy_str(out->natives.url, sizeof(out->natives.url), jstr(nat, "url", ""));
                    out->natives.size = jnum(nat, "size", 0);
                }
                replace_slashes(out->natives.path);
            }
            m->libCount++;
        }
    }
}

static MBVersionMeta* load_version_meta(Arena* a, const char* gameDir, const char* id,
                                        char* err, size_t errCap)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s\\versions\\%s\\%s.json", gameDir, id, id);
    size_t len = 0;
    char* text = read_text(a, path, &len);
    if (!text) {
        seterr(err, errCap, "找不到版本文件: %s", path);
        return NULL;
    }
    MBJson* root = mb_json_parse(a, text, len);
    if (!root || root->type != MB_JSON_OBJ) {
        seterr(err, errCap, "版本 JSON 解析失败: %s", path);
        return NULL;
    }
    if (mb_json_get(root, "inheritsFrom")) {
        seterr(err, errCap, "暂不支持带 inheritsFrom 的模组版本: %s", id);
        return NULL;
    }
    MBVersionMeta* meta = (MBVersionMeta*)mb_arena_calloc(a, sizeof(*meta));
    if (!meta) {
        seterr(err, errCap, "内存不足");
        return NULL;
    }
    parse_version_meta(root, meta);
    if (!meta->id[0]) copy_str(meta->id, sizeof(meta->id), id);
    if (!meta->mainClass[0]) {
        seterr(err, errCap, "版本缺少 mainClass: %s", id);
        return NULL;
    }
    return meta;
}

/* ========================================================================== */
/* version discovery                                                          */
/* ========================================================================== */

const char* mb_launcher_default_game_dir(char* buf, size_t cap)
{
    const char* appdata = getenv("APPDATA");
    if (!appdata || !*appdata) { if (buf && cap) buf[0] = '\0'; return buf; }
    snprintf(buf, cap, "%s\\.minecraft", appdata);
    return buf;
}

bool mb_launcher_dir_exists(const char* gameDir)
{
    return gameDir && is_dir(gameDir);
}

int mb_launcher_scan_versions(const char* gameDir, MBVersionEntry* out, int cap)
{
    int count = 0;
    char versions[1024];
    snprintf(versions, sizeof(versions), "%s\\versions", gameDir);
#ifdef _WIN32
    wchar_t w[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, versions, -1, w, 1024)) return 0;
    wchar_t pattern[1100];
    _snwprintf(pattern, 1100, L"%s\\*", w);
    WIN32_FIND_DATAW data;
    HANDLE h = FindFirstFileW(pattern, &data);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        char id[64];
        if (!WideCharToMultiByte(CP_UTF8, 0, data.cFileName, -1, id, sizeof(id), NULL, NULL)) continue;
        char json[1024];
        snprintf(json, sizeof(json), "%s\\versions\\%s\\%s.json", gameDir, id, id);
        if (!file_size_of(json, NULL)) continue;
        if (count < cap) {
            memset(&out[count], 0, sizeof(out[count]));
            copy_str(out[count].id, sizeof(out[count].id), id);
            copy_str(out[count].type, sizeof(out[count].type), "installed");
            out[count].installed = true;
            count++;
        }
    } while (FindNextFileW(h, &data));
    FindClose(h);
#else
    (void)versions;
#endif
    return count;
}

static bool fetch_json(Arena* a, const char* url, MBJson** out, char* err, size_t errCap)
{
    MBHttpRequest req;
    memset(&req, 0, sizeof(req));
    req.contentType = "application/json";
    MBLlmError e;
    memset(&e, 0, sizeof(e));
    MBHttpResponse resp;
    memset(&resp, 0, sizeof(resp));
    resp.error = &e;
    if (!mb_http_do(a, "GET", url, &req, &resp) || resp.status != 200 || !resp.body) {
        seterr(err, errCap, "无法获取版本清单（HTTP %ld）", resp.status);
        return false;
    }
    MBJson* root = mb_json_parse(a, resp.body, resp.bodyLen);
    if (!root) {
        seterr(err, errCap, "版本清单 JSON 解析失败");
        return false;
    }
    *out = root;
    return true;
}

bool mb_launcher_fetch_versions(Arena* a, const char* gameDir,
                                MBVersionEntry* out, int cap, int* count,
                                bool releasesOnly, char* err, size_t errCap)
{
    *count = 0;
    MBJson* root = NULL;
    if (!fetch_json(a, VERSION_MANIFEST_URL, &root, err, errCap)) return false;
    const MBJson* versions = mb_json_get(root, "versions");
    if (!versions || versions->type != MB_JSON_ARR) {
        seterr(err, errCap, "版本清单缺少 versions");
        return false;
    }
    for (size_t i = 0; i < versions->u.arr.count && *count < cap; i++) {
        const MBJson* v = mb_json_at(versions, i);
        if (!v) continue;
        const char* id = jstr(v, "id", "");
        const char* type = jstr(v, "type", "");
        if (!*id) continue;
        if (releasesOnly && strcmp(type, "release") != 0) continue;
        MBVersionEntry* e = &out[(*count)++];
        memset(e, 0, sizeof(*e));
        copy_str(e->id, sizeof(e->id), id);
        copy_str(e->type, sizeof(e->type), type);
        char json[1024];
        snprintf(json, sizeof(json), "%s\\versions\\%s\\%s.json", gameDir, id, id);
        e->installed = file_size_of(json, NULL);
    }
    return true;
}

/* ========================================================================== */
/* install                                                                    */
/* ========================================================================== */

static bool download_file(const char* url, const char* path,
                          char* err, size_t errCap)
{
    if (!need_download(path, 0)) return true;   /* existence is enough here */
    char dir[1024];
    copy_str(dir, sizeof(dir), path);
    char* slash = strrchr(dir, '\\');
    if (slash) { *slash = '\0'; if (!mkdirs(dir)) { seterr(err, errCap, "无法创建目录: %s", dir); return false; } }
    return mb_http_download(url, path, err, errCap);
}

static bool download_ref(const MBFileRef* ref, const char* path,
                         char* err, size_t errCap)
{
    if (!ref->url[0]) return true;
    if (!need_download(path, ref->size)) return true;
    return download_file(ref->url, path, err, errCap);
}

static bool extract_natives(const char* gameDir, const char* versionId,
                            MBLib* libs, int libCount, char* err, size_t errCap)
{
#ifdef _WIN32
    char natives[1024];
    snprintf(natives, sizeof(natives), "%s\\versions\\%s\\natives", gameDir, versionId);
    if (!mkdirs(natives)) { seterr(err, errCap, "无法创建 natives 目录"); return false; }
    for (int i = 0; i < libCount; i++) {
        if (!libs[i].nativesClass[0] || !libs[i].natives.path[0]) continue;
        char jar[1024];
        snprintf(jar, sizeof(jar), "%s\\libraries\\%s", gameDir, libs[i].natives.path);
        if (!file_size_of(jar, NULL)) continue;
        wchar_t* wjar = widen(jar);
        wchar_t* wnat = widen(natives);
        if (!wjar || !wnat) { free(wjar); free(wnat); continue; }
        wchar_t cmd[2300];
        _snwprintf(cmd, 2300, L"tar.exe -xf \"%s\" -C \"%s\"", wjar, wnat);
        bool ok = run_hidden(cmd, NULL);
        free(wjar); free(wnat);
        if (!ok) {
            seterr(err, errCap, "解压 natives 失败: %s\\%s", versionId, libs[i].nativesClass);
            return false;
        }
    }
    return true;
#else
    (void)gameDir; (void)versionId; (void)libs; (void)libCount;
    seterr(err, errCap, "当前平台不支持 natives 解压");
    return false;
#endif
}

bool mb_launcher_install(Arena* a, const char* gameDir, const char* versionId,
                         MBLauncherProgress progress, void* user,
                         char* err, size_t errCap)
{
    if (!mb_launcher_dir_exists(gameDir)) mkdirs(gameDir);

    MBJson* manifest = NULL;
    if (!fetch_json(a, VERSION_MANIFEST_URL, &manifest, err, errCap)) return false;
    const MBJson* versions = mb_json_get(manifest, "versions");
    const char* versionUrl = NULL;
    for (size_t i = 0; versions && i < versions->u.arr.count; i++) {
        const MBJson* v = mb_json_at(versions, i);
        if (v && strcmp(jstr(v, "id", ""), versionId) == 0) {
            versionUrl = jstr(v, "url", "");
            break;
        }
    }
    if (!versionUrl || !*versionUrl) {
        seterr(err, errCap, "Mojang 清单里没有版本 %s", versionId);
        return false;
    }

    char vdir[1024], jsonPath[1024], jarPath[1024];
    snprintf(vdir, sizeof(vdir), "%s\\versions\\%s", gameDir, versionId);
    snprintf(jsonPath, sizeof(jsonPath), "%s\\%s.json", vdir, versionId);
    snprintf(jarPath, sizeof(jarPath), "%s\\%s.jar", vdir, versionId);
    if (!mkdirs(vdir)) { seterr(err, errCap, "无法创建版本目录"); return false; }

    if (progress) progress(user, "下载版本信息", 0, 1, versionId);
    if (!download_file(versionUrl, jsonPath, err, errCap)) return false;

    MBVersionMeta* meta = load_version_meta(a, gameDir, versionId, err, errCap);
    if (!meta) return false;

    if (progress) progress(user, "下载客户端", 0, 1, versionId);
    if (meta->client.url[0] && !download_ref(&meta->client, jarPath, err, errCap)) return false;

    int libTotal = meta->libCount;
    for (int i = 0; i < meta->libCount; i++) {
        MBLib* lib = &meta->libs[i];
        char path[1200];
        if (lib->artifact.url[0]) {
            snprintf(path, sizeof(path), "%s\\libraries\\%s", gameDir, lib->artifact.path);
            if (!download_ref(&lib->artifact, path, err, errCap)) return false;
        }
        if (lib->natives.url[0]) {
            snprintf(path, sizeof(path), "%s\\libraries\\%s", gameDir, lib->natives.path);
            if (!download_ref(&lib->natives, path, err, errCap)) return false;
        }
        if (progress) progress(user, "下载依赖库", i + 1, libTotal, lib->name);
    }

    if (progress) progress(user, "解压 natives", 0, 1, versionId);
    if (!extract_natives(gameDir, versionId, meta->libs, meta->libCount, err, errCap)) return false;

    if (progress) progress(user, "下载资源索引", 0, 1, meta->assetIndexId);
    char indexDir[1024], indexPath[1024];
    snprintf(indexDir, sizeof(indexDir), "%s\\assets\\indexes", gameDir);
    snprintf(indexPath, sizeof(indexPath), "%s\\%s.json", indexDir, meta->assetIndexId);
    if (meta->assetIndexUrl[0] && need_download(indexPath, 0)) {
        if (!mkdirs(indexDir) || !download_file(meta->assetIndexUrl, indexPath, err, errCap)) return false;
    }

    size_t indexLen = 0;
    char* indexText = read_text(a, indexPath, &indexLen);
    MBJson* index = indexText ? mb_json_parse(a, indexText, indexLen) : NULL;
    const MBJson* objects = index ? mb_json_get(index, "objects") : NULL;
    int total = objects && objects->type == MB_JSON_OBJ ? (int)objects->u.obj.count : 0;
    int done = 0;
    for (int i = 0; i < total; i++) {
        const MBJson* obj = objects->u.obj.vals[i];
        const char* hash = jstr(obj, "hash", "");
        if (!*hash || strlen(hash) < 2) continue;
        char rel[256], dest[1400], url[1200];
        snprintf(rel, sizeof(rel), "assets\\objects\\%.2s\\%s", hash, hash);
        snprintf(dest, sizeof(dest), "%s\\%s", gameDir, rel);
        snprintf(url, sizeof(url), "%s%.2s/%s", RESOURCES_BASE, hash, hash);
        if (!download_file(url, dest, err, errCap)) return false;
        if (progress) progress(user, "下载资源", ++done, total, hash);
    }
    if (progress) progress(user, "安装完成", 1, 1, versionId);
    return true;
}

/* ========================================================================== */
/* Java discovery                                                             */
/* ========================================================================== */

static int java_version_of(const char* releasePath)
{
    FILE* f = fopen(releasePath, "rb");
    if (!f) return 0;
    char line[256];
    int major = 0;
    while (fgets(line, sizeof(line), f)) {
        const char* p = strstr(line, "JAVA_VERSION=\"");
        if (p) {
            p += 14;
            major = atoi(p);
            if (major == 1) {
                const char* dot = strchr(p, '.');
                if (dot) major = atoi(dot + 1);
            }
            break;
        }
    }
    fclose(f);
    return major;
}

static bool pick_bundled_java(const char* gameDir, int required, char* out, size_t cap)
{
    char runtimeDir[1024];
    snprintf(runtimeDir, sizeof(runtimeDir), "%s\\runtime", gameDir);
#ifdef _WIN32
    wchar_t w[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, runtimeDir, -1, w, 1024)) return false;
    wchar_t pattern[1100];
    _snwprintf(pattern, 1100, L"%s\\*", w);
    WIN32_FIND_DATAW data;
    HANDLE h = FindFirstFileW(pattern, &data);
    if (h == INVALID_HANDLE_VALUE) return false;

    int bestMajor = 0;
    char bestPath[1024] = "";
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        char name[128];
        if (!WideCharToMultiByte(CP_UTF8, 0, data.cFileName, -1, name, sizeof(name), NULL, NULL)) continue;
        char release[1200], java[1200];
        snprintf(release, sizeof(release), "%s\\%s\\release", runtimeDir, name);
        snprintf(java, sizeof(java), "%s\\%s\\bin\\javaw.exe", runtimeDir, name);
        if (!file_size_of(java, NULL)) continue;
        int major = java_version_of(release);
        if (!major) continue;
        if (required > 0 && major < required) continue;
        if (bestMajor == 0 || major < bestMajor) {
            bestMajor = major;
            copy_str(bestPath, sizeof(bestPath), java);
        }
    } while (FindNextFileW(h, &data));
    FindClose(h);
    if (bestPath[0]) { copy_str(out, cap, bestPath); return true; }
#else
    (void)runtimeDir;
#endif
    return false;
}

bool mb_launcher_java(Arena* a, const char* gameDir, int requiredMajor,
                      char* out, size_t cap)
{
    (void)a;
    out[0] = '\0';

    /* Old versions (or metadata without javaVersion) need the system Java 8.
     * Modern versions should use the runtime that shipped with the launcher. */
#ifdef _WIN32
    if (requiredMajor < 17) {
        wchar_t found0[1024];
        if (SearchPathW(NULL, L"javaw.exe", NULL, 1024, found0, NULL)) {
            char utf8[1024];
            if (WideCharToMultiByte(CP_UTF8, 0, found0, -1, utf8, sizeof(utf8), NULL, NULL)) {
                copy_str(out, cap, utf8);
                return true;
            }
        }
    }
#endif

    if (pick_bundled_java(gameDir, requiredMajor, out, cap)) return true;

#ifdef _WIN32
    wchar_t found[1024];
    if (SearchPathW(NULL, L"javaw.exe", NULL, 1024, found, NULL)) {
        char utf8[1024];
        if (WideCharToMultiByte(CP_UTF8, 0, found, -1, utf8, sizeof(utf8), NULL, NULL)) {
            copy_str(out, cap, utf8);
            return true;
        }
    }
#endif
    return false;
}

/* ========================================================================== */
/* launch                                                                     */
/* ========================================================================== */

static void offline_uuid(const char* name, char* out, size_t cap)
{
    unsigned char digest[16];
    memset(digest, 0, sizeof(digest));
#ifdef _WIN32
    BCRYPT_ALG_HANDLE alg = NULL;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, NULL, 0) >= 0) {
        BCRYPT_HASH_HANDLE hash = NULL;
        char seed[256];
        snprintf(seed, sizeof(seed), "OfflinePlayer:%s", name ? name : "");
        if (BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) >= 0) {
            BCryptHashData(hash, (PUCHAR)seed, (ULONG)strlen(seed), 0);
            BCryptFinishHash(hash, digest, 16, 0);
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
#endif
    digest[6] = (unsigned char)((digest[6] & 0x0F) | 0x30);
    digest[8] = (unsigned char)((digest[8] & 0x3F) | 0x80);
    snprintf(out, cap,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             digest[0], digest[1], digest[2], digest[3], digest[4], digest[5],
             digest[6], digest[7], digest[8], digest[9], digest[10], digest[11],
             digest[12], digest[13], digest[14], digest[15]);
}

static void replace_all(char* buf, size_t cap, const char* needle, const char* value)
{
    size_t nlen = strlen(needle);
    char* p = buf;
    while ((p = strstr(p, needle)) != NULL) {
        size_t tail = strlen(p + nlen);
        size_t vlen = strlen(value);
        if ((size_t)(p - buf) + vlen + tail + 1 > cap) break;
        memmove(p + vlen, p + nlen, tail + 1);
        memcpy(p, value, vlen);
        p += vlen;
    }
}

static bool tokenize_args(const char* text, char* out, size_t cap,
                          const char* player, const char* versionId,
                          const char* gameDir, const char* assetsDir,
                          const char* assetIndex, const char* uuid)
{
    char tmp[8192];
    copy_str(tmp, sizeof(tmp), text);
    out[0] = '\0';
    for (char* tok = strtok(tmp, " "); tok; tok = strtok(NULL, " ")) {
        char arg[1024];
        copy_str(arg, sizeof(arg), tok);
        replace_all(arg, sizeof(arg), "${auth_player_name}", player);
        replace_all(arg, sizeof(arg), "${version_name}", versionId);
        replace_all(arg, sizeof(arg), "${game_directory}", gameDir);
        replace_all(arg, sizeof(arg), "${assets_root}", assetsDir);
        replace_all(arg, sizeof(arg), "${assets_index_name}", assetIndex);
        replace_all(arg, sizeof(arg), "${auth_uuid}", uuid);
        replace_all(arg, sizeof(arg), "${auth_access_token}", "0");
        replace_all(arg, sizeof(arg), "${user_type}", "legacy");
        replace_all(arg, sizeof(arg), "${version_type}", "release");
        replace_all(arg, sizeof(arg), "${user_properties}", "{}");
        replace_all(arg, sizeof(arg), "${clientid}", "");
        replace_all(arg, sizeof(arg), "${auth_xuid}", "");
        replace_all(arg, sizeof(arg), "${resolution_width}", "1280");
        replace_all(arg, sizeof(arg), "${resolution_height}", "720");
        if (out[0]) append(out, cap, " ");
        append(out, cap, arg);
    }
    return true;
}

bool mb_launcher_launch(Arena* a, const MBLaunchOptions* opt,
                        char* err, size_t errCap)
{
    if (!opt || !opt->versionId[0]) { seterr(err, errCap, "没有选择版本"); return false; }
    if (!opt->playerName[0]) { seterr(err, errCap, "请先填写玩家名"); return false; }

    MBVersionMeta* meta = load_version_meta(a, opt->gameDir, opt->versionId, err, errCap);
    if (!meta) return false;

    char java[1024];
    if (!mb_launcher_java(a, opt->gameDir, meta->javaMajor, java, sizeof(java))) {
        seterr(err, errCap, "找不到可用的 Java（需要 Java %d）", meta->javaMajor);
        return false;
    }

    char natives[1024], versionJar[1200], assetsDir[1024];
    snprintf(natives, sizeof(natives), "%s\\versions\\%s\\natives",
             opt->gameDir, opt->versionId);
    snprintf(versionJar, sizeof(versionJar), "%s\\versions\\%s\\%s.jar",
             opt->gameDir, opt->versionId, opt->versionId);
    snprintf(assetsDir, sizeof(assetsDir), "%s\\assets", opt->gameDir);
    if (!file_size_of(versionJar, NULL)) {
        seterr(err, errCap, "客户端 jar 不存在，请先安装该版本");
        return false;
    }

    /* classpath: libraries then the client jar */
    char classpath[65536];
    classpath[0] = '\0';
    for (int i = 0; i < meta->libCount; i++) {
        if (!meta->libs[i].artifact.path[0] || meta->libs[i].nativesClass[0]) continue;
        char p[1200];
        snprintf(p, sizeof(p), "%s\\libraries\\%s", opt->gameDir, meta->libs[i].artifact.path);
        if (classpath[0]) append(classpath, sizeof(classpath), ";");
        append(classpath, sizeof(classpath), p);
    }
    if (classpath[0]) append(classpath, sizeof(classpath), ";");
    append(classpath, sizeof(classpath), versionJar);

    char uuid[64];
    offline_uuid(opt->playerName, uuid, sizeof(uuid));

    char gameArgs[8192];
    tokenize_args(meta->gameArgs, gameArgs, sizeof(gameArgs),
                  opt->playerName, opt->versionId, opt->gameDir,
                  assetsDir, meta->assetIndexId, uuid);

    /* A minimal but reliable JVM tail. Version JVM args are parsed for
       -XX options but the classpath is always ours. */
    char cmd[90000];
    cmd[0] = '\0';
    append(cmd, sizeof(cmd), "\"");
    append(cmd, sizeof(cmd), java);
    append(cmd, sizeof(cmd), "\" ");
    char mem[64];
    int memory = opt->memoryMb > 0 ? opt->memoryMb : 2048;
    snprintf(mem, sizeof(mem), "-Xmx%dM", memory);
    append(cmd, sizeof(cmd), mem);
    if (opt->demo) append(cmd, sizeof(cmd), " -Dminecraft.demo=true");
    append(cmd, sizeof(cmd), " -Djava.library.path=\"");
    append(cmd, sizeof(cmd), natives);
    append(cmd, sizeof(cmd), "\" -cp \"");
    append(cmd, sizeof(cmd), classpath);
    append(cmd, sizeof(cmd), "\" ");
    append(cmd, sizeof(cmd), meta->mainClass);
    append(cmd, sizeof(cmd), " ");
    append(cmd, sizeof(cmd), gameArgs);

#ifdef _WIN32
    wchar_t* wcmd = widen(cmd);
    wchar_t* wdir = widen(opt->gameDir);
    if (!wcmd || !wdir) {
        free(wcmd); free(wdir);
        seterr(err, errCap, "命令行编码失败");
        return false;
    }
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    BOOL ok = CreateProcessW(NULL, wcmd, NULL, NULL, FALSE, 0, NULL, wdir, &si, &pi);
    free(wcmd);
    free(wdir);
    if (!ok) {
        seterr(err, errCap, "启动游戏失败（错误码 %lu）", (unsigned long)GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    (void)cmd;
    seterr(err, errCap, "当前平台不支持启动 Minecraft");
    return false;
#endif
}
