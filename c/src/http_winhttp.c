/*
 * http_winhttp.c — the only platform specific module in the project.
 *
 * WinHTTP is used rather than WinINet because it is a plain library with no
 * user session state, and rather than libcurl because this project deliberately
 * has zero third party dependencies. If WinHTTP cannot even be opened (locked
 * down machine, unexpected proxy configuration), we fall back to the curl.exe
 * that ships with Windows 10 and later, so the AI feature still works.
 *
 * The public surface is deliberately tiny: mb_http_do(), mb_url_host() and
 * mb_url_join(). Everything else in the program is portable C11.
 */
#include "mb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#endif

/* ========================================================================== */
/* URL helpers (portable)                                                     */
/* ========================================================================== */

typedef struct {
    bool  secure;
    bool  valid;
    char  host[256];
    int   port;
    char  path[1024];
} ParsedUrl;

static ParsedUrl parse_url(const char* url)
{
    ParsedUrl u;
    memset(&u, 0, sizeof(u));
    u.port = 443;
    snprintf(u.path, sizeof(u.path), "/");

    const char* p = url;
    if (strncmp(p, "https://", 8) == 0) {
        u.secure = true;
        u.port = 443;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        u.secure = false;
        u.port = 80;
        p += 7;
    } else {
        return u;   /* only absolute http(s) URLs are supported */
    }

    /* host[:port] */
    size_t h = 0;
    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#' &&
           h + 1 < sizeof(u.host)) {
        u.host[h++] = *p++;
    }
    u.host[h] = '\0';
    if (h == 0) return u;

    if (*p == ':') {
        p++;
        int port = 0;
        while (*p >= '0' && *p <= '9') port = port * 10 + (*p++ - '0');
        if (port > 0 && port < 65536) u.port = port;
        else return u;
    }

    if (*p == '\0') {
        snprintf(u.path, sizeof(u.path), "/");
    } else if (*p == '?' || *p == '#') {
        snprintf(u.path, sizeof(u.path), "/%s", p);
    } else {
        snprintf(u.path, sizeof(u.path), "%s", p);
    }

    u.valid = true;
    return u;
}

/* ========================================================================== */
/* fast path: WinHTTP                                                         */
/* ========================================================================== */

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

static bool http_via_winhttp(Arena* a, const char* method, const char* url,
                             const MBHttpRequest* req, MBHttpResponse* out)
{
    ParsedUrl u = parse_url(url);
    if (!u.valid) {
        if (out->error) mb_llm_error_set(out->error, MB_LLM_ERR_NETWORK,
                                         "base URL 格式不对：%s", url);
        return false;
    }

    wchar_t* whost = widen(u.host);
    wchar_t* wpath = widen(u.path);
    wchar_t* wverb = widen(method);
    if (!whost || !wpath || !wverb) {
        free(whost); free(wpath); free(wverb);
        if (out->error) mb_llm_error_set(out->error, MB_LLM_ERR_NETWORK, "内存不足");
        return false;
    }

    /* request headers --------------------------------------------------- */
    wchar_t wheaders[1024];
    wheaders[0] = L'\0';
    {
        SBuf hb;
        mb_sb_init(&hb, a);
        mb_sb_printf(&hb, "Content-Type: %s\r\n",
                     req->contentType ? req->contentType : "application/json");
        if (req->authorization) mb_sb_printf(&hb, "Authorization: %s\r\n", req->authorization);
        char* h = mb_sb_done(&hb);
        wchar_t* wh = widen(h);
        if (wh) {
            wcsncpy(wheaders, wh, (sizeof(wheaders) / sizeof(wchar_t)) - 1);
            wheaders[(sizeof(wheaders) / sizeof(wchar_t)) - 1] = L'\0';
            free(wh);
        }
    }

    HINTERNET session = NULL, conn = NULL, hreq = NULL;
    bool ok = false;

    session = WinHttpOpen(L"Minecraft-Builder/1.0 (C edition)",
                          WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        session = WinHttpOpen(L"Minecraft-Builder/1.0 (C edition)",
                              WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session) goto done;

    WinHttpSetTimeouts(session, 15000, 20000, 60000, 180000);

    conn = WinHttpConnect(session, whost, (INTERNET_PORT)u.port, 0);
    if (!conn) goto done;

    hreq = WinHttpOpenRequest(conn, wverb, wpath, NULL, WINHTTP_NO_REFERER,
                              WINHTTP_DEFAULT_ACCEPT_TYPES,
                              u.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!hreq) goto done;

    if (wheaders[0]) {
        WinHttpAddRequestHeaders(hreq, wheaders, (DWORD)-1L,
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    if (!WinHttpSendRequest(hreq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            (LPVOID)(req->body ? req->body : NULL),
                            (DWORD)req->bodyLen, (DWORD)req->bodyLen, 0)) {
        goto done;
    }
    if (!WinHttpReceiveResponse(hreq, NULL)) goto done;

    /* status ------------------------------------------------------------- */
    {
        DWORD status = 0, size = sizeof(status);
        if (!WinHttpQueryHeaders(hreq,
                                 WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                 WINHTTP_NO_HEADER_INDEX)) {
            goto done;
        }
        out->status = (long)status;
    }

    /* body --------------------------------------------------------------- */
    {
        SBuf b;
        mb_sb_init(&b, a);
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(hreq, &avail)) goto done;
            if (avail == 0) break;
            char* chunk = (char*)mb_arena_alloc(a, avail + 1);
            DWORD read = 0;
            if (!WinHttpReadData(hreq, chunk, avail, &read)) goto done;
            if (read == 0) break;
            mb_sb_write(&b, chunk, read);
        }
        out->body = mb_sb_done(&b);
        out->bodyLen = strlen(out->body);
    }

    ok = true;

done:
    if (hreq) WinHttpCloseHandle(hreq);
    if (conn) WinHttpCloseHandle(conn);
    if (session) WinHttpCloseHandle(session);
    free(whost); free(wpath); free(wverb);
    return ok;
}

#else

static bool http_via_winhttp(Arena* a, const char* method, const char* url,
                             const MBHttpRequest* req, MBHttpResponse* out)
{
    (void)a; (void)method; (void)url; (void)req; (void)out;
    return false;
}

#endif /* _WIN32 */

/* ========================================================================== */
/* fallback: curl.exe, which ships with Windows 10 and later                  */
/* ========================================================================== */

static bool shell_arg_is_safe(const char* s)
{
    if (!s) return true;
    for (; *s; s++) {
        if (*s == '"' || *s == '\n' || *s == '\r' || *s == '`' || *s == '$') return false;
    }
    return true;
}

static char* read_whole_file(Arena* a, const char* path, size_t* outLen)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    SBuf b;
    mb_sb_init(&b, a);
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) mb_sb_write(&b, buf, n);
    fclose(f);
    char* s = mb_sb_done(&b);
    if (outLen) *outLen = strlen(s);
    return s;
}

static bool http_via_curl(Arena* a, const char* method, const char* url,
                          const MBHttpRequest* req, MBHttpResponse* out)
{
    if (!shell_arg_is_safe(url) || !shell_arg_is_safe(req->authorization)) return false;

    const char* tmp = getenv("TEMP");
    if (!tmp) tmp = ".";

    char bodyPath[600], respPath[600], statusPath[600];
    snprintf(bodyPath, sizeof(bodyPath), "%s\\mb_body_%lu.tmp", tmp,
             (unsigned long)GetCurrentProcessId());
    snprintf(respPath, sizeof(respPath), "%s\\mb_resp_%lu.tmp", tmp,
             (unsigned long)GetCurrentProcessId());
    snprintf(statusPath, sizeof(statusPath), "%s\\mb_stat_%lu.tmp", tmp,
             (unsigned long)GetCurrentProcessId());

    bool haveBody = req->body && req->bodyLen > 0;
    if (haveBody) {
        FILE* f = fopen(bodyPath, "wb");
        if (!f) return false;
        fwrite(req->body, 1, req->bodyLen, f);
        fclose(f);
    }

    /* Build the command piecewise so optional arguments do not need a forest of
       format strings. `-w "%{http_code}"` is redirected into a status file. */
    SBuf cmd;
    mb_sb_init(&cmd, a);
    mb_sb_printf(&cmd, "curl.exe -sS -L -m 180 -X %s -H \"Content-Type: %s\"",
                 method, req->contentType ? req->contentType : "application/json");
    if (req->authorization) {
        mb_sb_printf(&cmd, " -H \"Authorization: %s\"", req->authorization);
    }
    if (haveBody) {
        mb_sb_printf(&cmd, " --data-binary @\"%s\"", bodyPath);
    }
    mb_sb_printf(&cmd, " -o \"%s\" -w \"%%{http_code}\" \"%s\" > \"%s\" 2>nul",
                 respPath, url, statusPath);

    int rc = system(mb_sb_done(&cmd));

    if (rc != 0) {
        remove(bodyPath); remove(respPath); remove(statusPath);
        return false;
    }

    size_t statusLen = 0;
    char* statusText = read_whole_file(a, statusPath, &statusLen);
    remove(statusPath);

    size_t bodyLen = 0;
    char* body = read_whole_file(a, respPath, &bodyLen);
    remove(respPath);
    if (haveBody) remove(bodyPath);

    if (!statusText) return false;

    out->status = strtol(statusText, NULL, 10);
    out->body = body ? body : (char*)"";
    out->bodyLen = bodyLen;
    return true;
}

/* ========================================================================== */
/* public entry point                                                         */
/* ========================================================================== */

bool mb_http_do(Arena* a, const char* method, const char* url,
                const MBHttpRequest* req, MBHttpResponse* out)
{
    MBLlmError* err = out->error;    /* callers pre-load this; do not clobber it */
    memset(out, 0, sizeof(*out));
    out->error = err;

    if (http_via_winhttp(a, method, url, req, out)) return true;

    /* WinHTTP failed at the transport level. Ask curl before giving up. */
    out->status = 0;
    if (http_via_curl(a, method, url, req, out)) {
        if (out->error) out->error->kind = MB_LLM_ERR_NONE;
        return true;
    }

    if (out->error && out->error->kind == MB_LLM_ERR_NONE) {
        mb_llm_error_set(out->error, MB_LLM_ERR_NETWORK,
                         "无法连接到 %s。请检查 base URL、网络连通性或代理设置。", url);
    }
    return false;
}
