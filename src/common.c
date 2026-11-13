/* common.c - 状态、日志、协议编码 */
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

cmb_state_t g_state;
volatile LONG g_log_seq = 0;

static log_entry_t g_logs[CMB_LOG_LIMIT];
static int g_log_head = 0;
static int g_log_count = 0;

double now_sec(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (double)u.QuadPart / 1e7 - 11644473600.0;
}

void time_str(double t, char *out, int cap) {
    if (t <= 0) { snprintf(out, cap, "—"); return; }
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_s(&tm, &tt);
    strftime(out, cap, "%H:%M:%S", &tm);
}

void cmb_log(const char *level, const char *fmt, ...) {
    log_entry_t *e = &g_logs[g_log_head];
    e->t = now_sec();
    strncpy_s(e->level, sizeof(e->level), level, _TRUNCATE);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof(e->msg), fmt, ap);
    va_end(ap);
    g_log_head = (g_log_head + 1) % CMB_LOG_LIMIT;
    if (g_log_count < CMB_LOG_LIMIT) g_log_count++;
    InterlockedIncrement(&g_log_seq);
}

int cmb_log_snapshot(char *out, int cap) {
    int n = 0;
    n += snprintf(out + n, cap - n, "[");
    for (int i = 0; i < g_log_count && n < cap - 64; i++) {
        int idx = (g_log_head - g_log_count + i + CMB_LOG_LIMIT) % CMB_LOG_LIMIT;
        log_entry_t *e = &g_logs[idx];
        char ts[16]; time_str(e->t, ts, sizeof(ts));
        n += snprintf(out + n, cap - n, "%s{\"t\":%.3f,\"level\":\"%s\",\"msg\":\"",
                      i ? "," : "", e->t, e->level);
        for (char *p = e->msg; *p && n < cap - 8; p++) {
            if (*p == '"' || *p == '\\') out[n++] = '\\';
            out[n++] = *p;
        }
        n += snprintf(out + n, cap - n, "\"}");
    }
    n += snprintf(out + n, cap - n, "]");
    return n;
}

int cmb_log_tail_text(char *out, int cap, int max_lines) {
    if (max_lines <= 0 || g_log_count == 0) { if (cap > 0) out[0] = 0; return 0; }
    int start = g_log_count - max_lines;
    if (start < 0) start = 0;
    int n = 0;
    for (int i = start; i < g_log_count && n < cap - 64; i++) {
        int idx = (g_log_head - g_log_count + i + CMB_LOG_LIMIT) % CMB_LOG_LIMIT;
        log_entry_t *e = &g_logs[idx];
        char ts[16]; time_str(e->t, ts, sizeof(ts));
        n += snprintf(out + n, cap - n, "%s  %-5s %s\n", ts, e->level, e->msg);
    }
    return n;
}

void proto_mouse(char *out, int cap, int dx, int dy, const int buttons[3], int wheel,
                 bool absolute, int x, int y) {
    if (absolute) {
        snprintf(out, cap, "M,%d,%d,1", x, y);
    } else {
        snprintf(out, cap, "M,%d,%d,%d,%d,%d,%d",
                 dx, dy, buttons[0], buttons[1], buttons[2], wheel);
    }
}

void proto_key(char *out, int cap, bool down, int code, int mods) {
    snprintf(out, cap, "K,%s,%d,%d", down ? "down" : "up", code, mods);
}
