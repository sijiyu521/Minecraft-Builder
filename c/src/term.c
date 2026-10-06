/*
 * term.c — console setup, true colour output and raw key input.
 *
 * The renderer produces ANSI escape sequences, so on Windows the console has to
 * be told three things: speak UTF-8, honour virtual terminal sequences, and give
 * us keystrokes one at a time instead of line by line. All of that lives here so
 * that render.c and main.c stay portable.
 *
 * Input goes through ReadConsoleInput so that window resizes arrive as events
 * rather than as garbled key presses.
 */
#include "mb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

/* ========================================================================== */
/* Windows implementation                                                     */
/* ========================================================================== */

#ifdef _WIN32

static DWORD g_savedOutMode = 0;
static DWORD g_savedInMode = 0;
static bool  g_inited = false;

/* Bytes decoded from a non-ASCII key press, handed out one at a time so that
   the caller can treat them as ordinary text input. */
static char g_pending[8];
static int  g_pendingLen = 0;
static int  g_pendingPos = 0;

void mb_term_init(void)
{
    if (g_inited) return;

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);

    if (out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &g_savedOutMode)) {
        DWORD mode = g_savedOutMode
                   | ENABLE_VIRTUAL_TERMINAL_PROCESSING
                   | ENABLE_PROCESSED_OUTPUT;
        SetConsoleMode(out, mode);
    }

    if (in != INVALID_HANDLE_VALUE && GetConsoleMode(in, &g_savedInMode)) {
        DWORD mode = g_savedInMode;
        mode |= ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS;
        mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT
                  | ENABLE_MOUSE_INPUT | ENABLE_QUICK_EDIT_MODE);
        SetConsoleMode(in, mode);
    }

    g_inited = true;
}

void mb_term_restore(void)
{
    if (!g_inited) return;

    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);

    /* show the cursor and drop every attribute the renderer may have set */
    fputs("\x1b[0m\x1b[?25h", stdout);
    fflush(stdout);

    if (out != INVALID_HANDLE_VALUE) SetConsoleMode(out, g_savedOutMode);
    if (in != INVALID_HANDLE_VALUE) SetConsoleMode(in, g_savedInMode);
    g_inited = false;
}

void mb_term_size(int* cols, int* rows)
{
    *cols = 100;
    *rows = 30;

    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (out != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(out, &info)) {
        int c = info.srWindow.Right - info.srWindow.Left + 1;
        int r = info.srWindow.Bottom - info.srWindow.Top + 1;
        if (c > 20) *cols = c;
        if (r > 8)  *rows = r;
    }
}

void mb_term_write(const char* s)
{
    fputs(s, stdout);
    fflush(stdout);
}

static int decode_utf8_into_pending(WCHAR ch)
{
    unsigned int cp = (unsigned int)ch;
    int n = 0;

    if (cp < 0x80) {
        g_pending[n++] = (char)cp;
    } else if (cp < 0x800) {
        g_pending[n++] = (char)(0xC0 | (cp >> 6));
        g_pending[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp >= 0xD800 && cp <= 0xDBFF) {
        /* Lone high surrogate: remember it and wait for the low half. */
        g_pending[n++] = (char)0xEF;   /* U+FFFD REPLACEMENT CHARACTER */
        g_pending[n++] = (char)0xBF;
        g_pending[n++] = (char)0xBD;
    } else {
        g_pending[n++] = (char)(0xE0 | (cp >> 12));
        g_pending[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        g_pending[n++] = (char)(0x80 | (cp & 0x3F));
    }

    g_pendingLen = n;
    g_pendingPos = 0;
    return n;
}

static int map_virtual_key(WORD vk, WCHAR ch, bool ctrl)
{
    if (ctrl && (ch == 3 || vk == 'C')) return MB_KEY_CTRL_C;

    switch (vk) {
    case VK_ESCAPE:  return MB_KEY_ESC;
    case VK_RETURN:  return MB_KEY_ENTER;
    case VK_TAB:     return MB_KEY_TAB;
    case VK_BACK:    return MB_KEY_BACKSPACE;
    case VK_UP:      return MB_KEY_UP;
    case VK_DOWN:    return MB_KEY_DOWN;
    case VK_LEFT:    return MB_KEY_LEFT;
    case VK_RIGHT:   return MB_KEY_RIGHT;
    case VK_PRIOR:   return MB_KEY_PAGEUP;
    case VK_NEXT:    return MB_KEY_PAGEDOWN;
    case VK_HOME:    return MB_KEY_HOME;
    case VK_END:     return MB_KEY_END;
    default:         break;
    }

    if (ch == 0) return MB_KEY_NONE;
    decode_utf8_into_pending(ch);
    return MB_KEY_CHAR;
}

int mb_term_read_key(void)
{
    if (g_pendingPos < g_pendingLen) {
        return MB_KEY_CHAR + (unsigned char)g_pending[g_pendingPos++];
    }

    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (in == INVALID_HANDLE_VALUE) return MB_KEY_NONE;

    INPUT_RECORD rec;
    DWORD read = 0;

    for (;;) {
        if (!ReadConsoleInputW(in, &rec, 1, &read) || read == 0) return MB_KEY_NONE;

        if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT) return MB_KEY_RESIZE;

        if (rec.EventType != KEY_EVENT) continue;
        if (!rec.Event.KeyEvent.bKeyDown) continue;

        WORD vk = rec.Event.KeyEvent.wVirtualKeyCode;
        WCHAR ch = rec.Event.KeyEvent.uChar.UnicodeChar;
        bool ctrl = (rec.Event.KeyEvent.dwControlKeyState & (LEFT_CTRL_PRESSED |
                                                             RIGHT_CTRL_PRESSED)) != 0;

        int key = map_virtual_key(vk, ch, ctrl);
        if (key == MB_KEY_CHAR) {
            return MB_KEY_CHAR + (unsigned char)g_pending[g_pendingPos++];
        }
        if (key != MB_KEY_NONE) return key;
    }
}

char* mb_term_input_line(Arena* a, const char* label)
{
    /* Hand the console back to the user's shell settings for one line. */
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);

    fputs("\x1b[0m", stdout);
    if (label) {
        fputs(label, stdout);
    }
    fflush(stdout);

    if (in != INVALID_HANDLE_VALUE) SetConsoleMode(in, g_savedInMode);
    if (out != INVALID_HANDLE_VALUE) SetConsoleMode(out, g_savedOutMode);
    SetConsoleOutputCP(CP_UTF8);

    char buf[1024];
    buf[0] = '\0';
    if (!fgets(buf, sizeof(buf), stdin)) buf[0] = '\0';

    /* Back to raw mode. */
    if (out != INVALID_HANDLE_VALUE) {
        DWORD mode = g_savedOutMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING
                   | ENABLE_PROCESSED_OUTPUT;
        SetConsoleMode(out, mode);
    }
    if (in != INVALID_HANDLE_VALUE) {
        DWORD mode = g_savedInMode | ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS;
        mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT
                  | ENABLE_MOUSE_INPUT | ENABLE_QUICK_EDIT_MODE);
        SetConsoleMode(in, mode);
    }

    /* strip CR/LF */
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';

    g_pendingLen = g_pendingPos = 0;
    return mb_arena_strdup(a, buf);
}

/* ========================================================================== */
/* POSIX implementation                                                       */
/* ========================================================================== */

#else

static struct termios g_savedTermios;
static bool           g_inited = false;

void mb_term_init(void)
{
    if (g_inited) return;
    if (tcgetattr(STDIN_FILENO, &g_savedTermios) == 0) {
        struct termios raw = g_savedTermios;
        raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
    fputs("\x1b[?25l", stdout);
    fflush(stdout);
    g_inited = true;
}

void mb_term_restore(void)
{
    if (!g_inited) return;
    fputs("\x1b[0m\x1b[?25h", stdout);
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSANOW, &g_savedTermios);
    g_inited = false;
}

void mb_term_size(int* cols, int* rows)
{
    struct winsize ws;
    *cols = 100;
    *rows = 30;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 20) {
        *cols = ws.ws_col;
        *rows = ws.ws_row > 8 ? ws.ws_row : 30;
    }
}

void mb_term_write(const char* s)
{
    fputs(s, stdout);
    fflush(stdout);
}

int mb_term_read_key(void)
{
    unsigned char c = 0;
    if (read(STDIN_FILENO, &c, 1) != 1) return MB_KEY_NONE;

    if (c == 3) return MB_KEY_CTRL_C;
    if (c == '\r' || c == '\n') return MB_KEY_ENTER;
    if (c == '\t') return MB_KEY_TAB;
    if (c == 127 || c == 8) return MB_KEY_BACKSPACE;
    if (c == 27) return MB_KEY_ESC;
    return MB_KEY_CHAR + c;
}

char* mb_term_input_line(Arena* a, const char* label)
{
    if (label) fputs(label, stdout);
    fflush(stdout);

    tcsetattr(STDIN_FILENO, TCSANOW, &g_savedTermios);

    char buf[1024];
    buf[0] = '\0';
    if (!fgets(buf, sizeof(buf), stdin)) buf[0] = '\0';

    struct termios raw = g_savedTermios;
    raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    return mb_arena_strdup(a, buf);
}

#endif /* _WIN32 */
