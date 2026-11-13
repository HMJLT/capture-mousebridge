/* mcp.c - 内置 MCP: 工具分发 + 低 token 状态摘要
 * 协议: JSON-RPC 2.0 over HTTP POST /mcp
 *   请求: {"tool":"status","args":{...}}  或  {"method":"tools/status","arguments":{...}}
 *   响应: {"jsonrpc":"2.0","id":...,"result":{...}}
 * 极简 JSON 取值: 只覆盖本程序自身产生的扁平/嵌套对象, 不做通用解析器。
 */
#include "mcp.h"
#include "capture.h"
#include "serial_link.h"
#include "input_bridge.h"
#include "link.h"
#include "gui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---- 极简 JSON 取值工具 ---- */

static const char *json_find_key(const char *json, const char *key) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p && (*p == ':' || isspace((unsigned char)*p))) p++;
    return p;
}

static void json_get_str(const char *json, const char *key, char *out, int cap) {
    out[0] = 0;
    const char *p = json_find_key(json, key);
    if (!p || *p != '"') return;
    p++;
    int n = 0;
    while (*p && *p != '"' && n < cap - 1) {
        if (*p == '\\' && p[1]) { p++; }
        out[n++] = *p++;
    }
    out[n] = 0;
}

static int json_get_int(const char *json, const char *key, int def) {
    const char *p = json_find_key(json, key);
    if (!p) return def;
    return (int)strtol(p, NULL, 10);
}

static bool json_get_bool(const char *json, const char *key, bool def) {
    const char *p = json_find_key(json, key);
    if (!p) return def;
    return strncmp(p, "true", 4) == 0;
}

static void json_escape(const char *in, char *out, int cap) {
    int n = 0;
    for (const char *p = in; *p && n < cap - 8; p++) {
        if (*p == '"' || *p == '\\') out[n++] = '\\';
        out[n++] = *p;
    }
    out[n] = 0;
}

/* ---- 状态 / 向导序列化 ---- */

static void write_status(char *out, int cap) {
    char ts[16]; time_str(g_state.last_event_at, ts, sizeof(ts));
    char link_dev[CMB_MAX_PATH]; json_escape(g_state.link_device, link_dev, sizeof(link_dev));
    char link_err[256];          json_escape(g_state.link_error, link_err, sizeof(link_err));
    char cap_err[256];           json_escape(g_state.cap_error, cap_err, sizeof(cap_err));

    int n = 0;
    n += snprintf(out + n, cap - n,
        "{\"uptime_s\":%.1f,\"link\":{\"state\":%d,\"channel\":%d,\"device\":\"%s\","
        "\"baud\":%d,\"error\":\"%s\",\"attempts\":%d},\"video\":{\"capturing\":%d,"
        "\"w\":%d,\"h\":%d,\"fps\":%.1f,\"frames\":%ld,\"buffer_ms\":%d,\"error\":\"%s\"},"
        "\"input\":{\"grabbed\":%d,\"sent\":%ld,\"dropped\":%ld,\"keys\":[",
        now_sec() - g_state.boot_time,
        g_state.link_state, g_state.channel, link_dev, g_state.link_baud, link_err,
        g_state.reconnect_attempts,
        g_state.capturing, g_state.cap_width, g_state.cap_height, g_state.cap_fps,
        g_state.cap_frames, g_state.cap_latency_ms, cap_err,
        g_state.grabbed, g_state.events_sent, g_state.events_dropped);
    for (int i = 0; i < g_state.key_count; i++)
        n += snprintf(out + n, cap - n, "%s%d", i ? "," : "", g_state.keys_pressed[i]);
    char ep[192];
    json_escape(g_state.mcp_endpoint, ep, sizeof(ep));
    char uhost[80]; json_escape(g_state.udp_host, uhost, sizeof(uhost));
    char urep[128]; json_escape(g_state.udp_reply, urep, sizeof(urep));
    n += snprintf(out + n, cap - n,
        "],\"last_event\":\"%s\"},\"udp\":{\"host\":\"%s\",\"port\":%d,\"interval_ms\":%d,"
        "\"tx\":%d,\"rx\":%d,\"hid_mounted\":%s,\"discovering\":%s,\"board\":\"%s\"},"
        "\"mcp\":{\"endpoint\":\"%s\",\"lan_enabled\":%s,"
        "\"lan_port\":%d,\"lan_state\":%d,\"lan_clients\":%d}}",
        ts, uhost, g_state.udp_port, g_state.udp_interval_ms,
        g_state.udp_tx, g_state.udp_rx,
        g_state.udp_hid ? "true" : "false",
        g_state.udp_discovering ? "true" : "false",
        urep, ep, g_state.lan_enabled ? "true" : "false",
        g_state.lan_port, g_state.lan_state, g_state.lan_clients);
}

static void write_guide(char *out, int cap) {
    /* 分步检查: 返回每步 ok 与卡点 */
    bool s1 = g_state.capturing;
    bool s2 = g_state.link_device[0] != 0;
    bool s3 = g_state.link_state == LINK_CONNECTED;
    bool s4 = g_state.last_ok_at > 0;
    bool s5 = g_state.grabbed;

    int blocked = 0;
    if (!s1) blocked = 1; else if (!s2) blocked = 2;
    else if (!s3) blocked = 3; else if (!s4) blocked = 4; else if (!s5) blocked = 5;

    char t4[16]; time_str(g_state.last_ok_at, t4, sizeof(t4));
    int n = 0;
    n += snprintf(out + n, cap - n,
        "{\"auto_reconnect\":%s,\"interval_s\":%d,\"blocked_at\":%d,\"steps\":[",
        g_state.auto_reconnect ? "true" : "false", g_state.reconnect_interval, blocked);
    const char *titles[5] = {"检查采集卡硬件", "确认 ESP32 上电与枚举",
                             g_state.channel == CH_UDP ? "确认板子 IP 并连上 UDP 4210" :
                             g_state.channel == CH_SERIAL ? "选择串口与波特率" : "本机注入无需链路",
                             "链路心跳", "接管键鼠并验证"};
    bool oks[5] = {s1, s2, s3, s4, s5};
    for (int i = 0; i < 5; i++) {
        n += snprintf(out + n, cap - n, "%s{\"n\":%d,\"title\":\"%s\",\"ok\":%s}",
                      i ? "," : "", i + 1, titles[i], oks[i] ? "true" : "false");
    }
    n += snprintf(out + n, cap - n, "],\"last_ok\":\"%s\"}", t4);
}

static void write_serial_ports(char *out, int cap) {
    serial_port_t ports[CMB_MAX_PORTS];
    int cnt = serial_enumerate(ports, CMB_MAX_PORTS);
    int n = snprintf(out, cap, "{\"ports\":[");
    for (int i = 0; i < cnt; i++) {
        char desc[256]; json_escape(ports[i].description, desc, sizeof(desc));
        n += snprintf(out + n, cap - n, "%s{\"port\":\"%s\",\"description\":\"%s\"}",
                      i ? "," : "", ports[i].port, desc);
    }
    snprintf(out + n, cap - n, "]}");
}

static void write_video_devs(char *out, int cap) {
    video_dev_t devs[10];
    int cnt = capture_enum(devs, 10);
    int n = snprintf(out, cap, "{\"devices\":[");
    for (int i = 0; i < cnt; i++) {
        n += snprintf(out + n, cap - n, "%s{\"index\":%d,\"name\":\"%s\"}",
                      i ? "," : "", devs[i].index, devs[i].name);
    }
    snprintf(out + n, cap - n, "]}");
}

static void write_config(char *out, int cap) {
    char sp[CMB_MAX_PATH], ep[192];
    json_escape(g_state.serial_port, sp, sizeof(sp));
    json_escape(g_state.mcp_endpoint, ep, sizeof(ep));
    int n = snprintf(out, cap, "{\"config\":{");
    n += snprintf(out + n, cap - n,
        "\"video_source\":%d,\"video_width\":%d,\"video_height\":%d,\"fps_cap\":%d,"
        "\"channel\":%d,\"serial_port\":\"%s\",\"serial_baud\":%d,"
        "\"capture_enabled\":%s,\"grabbed\":%s,"
        "\"auto_reconnect\":%s,\"reconnect_interval\":%d,"
        "\"mcp_endpoint\":\"%s\",\"lan_enabled\":%s,\"lan_port\":%d",
        g_state.video_source, g_state.video_width, g_state.video_height, g_state.fps_cap,
        g_state.channel, sp, g_state.serial_baud,
        g_state.capture_enabled ? "true" : "false", g_state.grabbed ? "true" : "false",
        g_state.auto_reconnect ? "true" : "false", g_state.reconnect_interval,
        ep, g_state.lan_enabled ? "true" : "false", g_state.lan_port);
    snprintf(out + n, cap - n, "}}");
}

/* ---- 工具分发 ---- */

void mcp_handle(const char *body, char *out, int cap) {
    char tool[64] = "status";
    char args_body[CMB_MAX_JSON] = "";

    const char *p = json_find_key(body, "tool");
    if (!p) p = json_find_key(body, "method");
    if (p) {
        /* method 形如 tools/status -> 取末段 */
        if (*p == '"') {
            char m[64]; json_get_str(body, "tool", m, sizeof(m));
            if (!m[0]) json_get_str(body, "method", m, sizeof(m));
            char *slash = strrchr(m, '/');
            strncpy(tool, slash ? slash + 1 : m, sizeof(tool) - 1);
        }
    }
    /* 提取 arguments/args 对象体 (浅提取到配对大括号) */
    const char *args = json_find_key(body, "arguments");
    if (!args) args = json_find_key(body, "args");
    if (args && *args == '{') {
        int depth = 0; const char *q = args;
        while (*q) {
            if (*q == '{') depth++;
            if (*q == '}') { depth--; if (depth == 0) { q++; break; } }
            q++;
        }
        int len = (int)(q - args);
        if (len > (int)sizeof(args_body) - 1) len = sizeof(args_body) - 1;
        memcpy(args_body, args, len);
        args_body[len] = 0;
    }

    char result[CMB_MAX_JSON];
    result[0] = 0;

    if (strcmp(tool, "status") == 0) {
        write_status(result, sizeof(result));
    } else if (strcmp(tool, "video_devices") == 0) {
        write_video_devs(result, sizeof(result));
    } else if (strcmp(tool, "video_open") == 0) {
        int idx = json_get_int(args_body, "index", 0);
        int w = json_get_int(args_body, "w", 0);
        int h = json_get_int(args_body, "h", 0);
        int fps = json_get_int(args_body, "fps", g_state.fps_cap);
        bool ok = capture_start(idx, w, h, fps);
        snprintf(result, sizeof(result), "{\"ok\":%s,\"index\":%d,\"error\":\"%s\"}",
                 ok ? "true" : "false", idx, g_state.cap_error);
        gui_request_repaint();
    } else if (strcmp(tool, "video_close") == 0) {
        capture_stop();
        snprintf(result, sizeof(result), "{\"ok\":true}");
    } else if (strcmp(tool, "serial_ports") == 0) {
        write_serial_ports(result, sizeof(result));
    } else if (strcmp(tool, "connect") == 0) {
        char port[CMB_MAX_PATH]; json_get_str(args_body, "serial_port", port, sizeof(port));
        char uhost[64];          json_get_str(args_body, "udp_host", uhost, sizeof(uhost));
        int baud = json_get_int(args_body, "serial_baud", g_state.serial_baud);
        int uport = json_get_int(args_body, "udp_port", g_state.udp_port);
        int ch = json_get_int(args_body, "channel", g_state.channel);
        if (g_state.link_state != LINK_IDLE) link_disconnect();
        if (port[0]) strncpy(g_state.serial_port, port, sizeof(g_state.serial_port) - 1);
        if (uhost[0]) strncpy(g_state.udp_host, uhost, sizeof(g_state.udp_host) - 1);
        if (uport > 0 && uport <= 65535) g_state.udp_port = uport;
        g_state.serial_baud = baud;
        g_state.channel = (channel_t)ch;
        bool ok = link_connect();
        snprintf(result, sizeof(result), "{\"ok\":%s,\"link_state\":%d,\"channel\":%d}",
                 ok ? "true" : "false", g_state.link_state, g_state.channel);
    } else if (strcmp(tool, "discover") == 0) {
        char ip[64] = ""; char st[512] = ""; int hid = 0;
        extern bool udp_discover_now(char *, int, int *, char *, int);
        bool ok = udp_discover_now(ip, sizeof(ip), &hid, st, sizeof(st));
        if (ok) {
            strncpy(g_state.udp_host, ip, sizeof(g_state.udp_host) - 1);
            g_state.udp_hid = hid;
        }
        snprintf(result, sizeof(result),
                 "{\"ok\":%s,\"ip\":\"%s\",\"hid_mounted\":%s,\"port\":%d}",
                 ok ? "true" : "false", ip, hid ? "true" : "false", g_state.udp_port);
    } else if (strcmp(tool, "disconnect") == 0) {
        link_disconnect();
        snprintf(result, sizeof(result), "{\"ok\":true}");
    } else if (strcmp(tool, "reconnect_guide") == 0) {
        write_guide(result, sizeof(result));
    } else if (strcmp(tool, "set_grab") == 0) {
        bool grabbed = json_get_bool(args_body, "grabbed", g_state.grabbed);
        if (!grabbed) input_release_all();
        g_state.grabbed = grabbed;
        snprintf(result, sizeof(result), "{\"grabbed\":%s}", g_state.grabbed ? "true" : "false");
    } else if (strcmp(tool, "send_mouse") == 0) {
        mouse_ev_t e; ZeroMemory(&e, sizeof(e));
        e.dx = json_get_int(args_body, "dx", 0);
        e.dy = json_get_int(args_body, "dy", 0);
        e.wheel = json_get_int(args_body, "wheel", 0);
        e.absolute = json_get_bool(args_body, "absolute", false);
        e.x = json_get_int(args_body, "x", 0);
        e.y = json_get_int(args_body, "y", 0);
        e.buttons[0] = json_get_int(args_body, "b0", 0);
        e.buttons[1] = json_get_int(args_body, "b1", 0);
        e.buttons[2] = json_get_int(args_body, "b2", 0);
        input_dispatch_mouse(&e);
        snprintf(result, sizeof(result), "{\"ok\":true,\"sent\":%ld}", g_state.events_sent);
    } else if (strcmp(tool, "send_key") == 0) {
        key_ev_t e; ZeroMemory(&e, sizeof(e));
        e.code = json_get_int(args_body, "code", 0);
        json_get_str(args_body, "code_name", e.code_name, sizeof(e.code_name));
        e.down = json_get_bool(args_body, "down", true);
        e.ctrl = json_get_bool(args_body, "ctrl", false);
        e.shift = json_get_bool(args_body, "shift", false);
        e.alt = json_get_bool(args_body, "alt", false);
        e.meta = json_get_bool(args_body, "meta", false);
        input_dispatch_key(&e);
        snprintf(result, sizeof(result), "{\"ok\":true,\"sent\":%ld}", g_state.events_sent);
    } else if (strcmp(tool, "send_text") == 0) {
        /* 按 ASCII 字符串逐字符敲键盘: 省得 agent 一次一次调 send_key */
        char txt[1024] = "";
        json_get_str(args_body, "text", txt, sizeof(txt));
        int ok = 1, typed = 0;
        for (unsigned i = 0; txt[i] && i < sizeof(txt); i++) {
            unsigned char c = (unsigned char)txt[i];
            int vk = 0, sh = 0;
            if (c >= 'a' && c <= 'z') vk = (int)(c - 32);
            else if (c >= 'A' && c <= 'Z') { vk = (int)c; sh = 1; }
            else if (c >= '0' && c <= '9') vk = (int)c;
            else {
                switch (c) {
                case ' ': vk = 0x20; break;
                case '\t': vk = 0x09; break;
                case '\n': vk = 0x0D; break;
                case '.': vk = 0xBE; break;
                case ',': vk = 0xBC; break;
                case '/': vk = 0xBF; break;
                case ';': vk = 0xBA; break;
                case '\'': vk = 0xDE; break;
                case '-': vk = 0xBD; break;
                case '=': vk = 0xBB; break;
                case '[': vk = 0xDB; break;
                case ']': vk = 0xDD; break;
                case '\\': vk = 0xDC; break;
                case '`': vk = 0xC0; break;
                case '~': vk = 0xC0; sh = 1; break;
                case '!': vk = '1'; sh = 1; break;
                case '@': vk = '2'; sh = 1; break;
                case '#': vk = '3'; sh = 1; break;
                case '$': vk = '4'; sh = 1; break;
                case '%': vk = '5'; sh = 1; break;
                case '^': vk = '6'; sh = 1; break;
                case '&': vk = '7'; sh = 1; break;
                case '*': vk = '8'; sh = 1; break;
                case '(': vk = '9'; sh = 1; break;
                case ')': vk = '0'; sh = 1; break;
                case '_': vk = 0xBD; sh = 1; break;
                case '+': vk = 0xBB; sh = 1; break;
                case '{': vk = 0xDB; sh = 1; break;
                case '}': vk = 0xDD; sh = 1; break;
                case '|': vk = 0xDC; sh = 1; break;
                case ':': vk = 0xBA; sh = 1; break;
                case '"': vk = 0xDE; sh = 1; break;
                case '<': vk = 0xBC; sh = 1; break;
                case '>': vk = 0xBE; sh = 1; break;
                case '?': vk = 0xBF; sh = 1; break;
                default: vk = 0; break;
                }
            }
            if (!vk) { ok = 0; continue; }
            key_ev_t ke; ZeroMemory(&ke, sizeof(ke));
            ke.code = vk; ke.shift = sh; ke.down = true;
            input_dispatch_key(&ke);
            ke.down = false;
            input_dispatch_key(&ke);
            typed++;
            Sleep(6);   /* 板子 HID 节奏 4ms, 太快会丢字 */
        }
        snprintf(result, sizeof(result), "{\"ok\":%s,\"chars\":%d,\"grabbed\":%s}",
                 ok ? "true" : "false", typed, g_state.grabbed ? "true" : "false");
    } else if (strcmp(tool, "snapshot") == 0) {
        /* 原分辨率存 BMP: agent 不用截图整屏也能核对被控端画面 */
        char path[512] = "";
        json_get_str(args_body, "path", path, sizeof(path));
        bool ok = capture_save_bmp(path);
        snprintf(result, sizeof(result),
                 "{\"ok\":%s,\"path\":\"%s\",\"w\":%d,\"h\":%d}", ok ? "true" : "false",
                 path, g_state.cap_width, g_state.cap_height);
    } else if (strcmp(tool, "logs") == 0) {
        int limit = json_get_int(args_body, "limit", 20);
        char *p = result; int n = 0;
        n += snprintf(p + n, sizeof(result) - n, "{\"logs\":");
        n += cmb_log_snapshot(p + n, sizeof(result) - n);
        snprintf(p + n, sizeof(result) - n, "}");
        (void)limit;
    } else if (strcmp(tool, "config_get") == 0) {
        write_config(result, sizeof(result));
    } else if (strcmp(tool, "config_set") == 0) {
        /* 热更新已知标量 */
        if (strstr(args_body, "video_source")) g_state.video_source = json_get_int(args_body, "video_source", g_state.video_source);
        if (strstr(args_body, "fps_cap")) g_state.fps_cap = json_get_int(args_body, "fps_cap", g_state.fps_cap);
        if (strstr(args_body, "serial_baud")) g_state.serial_baud = json_get_int(args_body, "serial_baud", g_state.serial_baud);
        if (strstr(args_body, "reconnect_interval")) g_state.reconnect_interval = json_get_int(args_body, "reconnect_interval", g_state.reconnect_interval);
        if (strstr(args_body, "auto_reconnect")) g_state.auto_reconnect = json_get_bool(args_body, "auto_reconnect", g_state.auto_reconnect);
        if (strstr(args_body, "udp_port")) g_state.udp_port = json_get_int(args_body, "udp_port", g_state.udp_port);
        if (strstr(args_body, "udp_interval_ms")) g_state.udp_interval_ms = json_get_int(args_body, "udp_interval_ms", g_state.udp_interval_ms);
        if (strstr(args_body, "udp_host")) {
            char uh[64]; json_get_str(args_body, "udp_host", uh, sizeof(uh));
            if (uh[0]) strncpy(g_state.udp_host, uh, sizeof(g_state.udp_host) - 1);
        }
        char sp[CMB_MAX_PATH]; json_get_str(args_body, "serial_port", sp, sizeof(sp));
        if (sp[0]) strncpy(g_state.serial_port, sp, sizeof(g_state.serial_port) - 1);
        write_config(result, sizeof(result));
    } else {
        snprintf(result, sizeof(result), "{\"error\":\"unknown tool: %s\"}", tool);
    }

    /* 包成 JSON-RPC 信封; id 原样透传 */
    /* 包成 JSON-RPC 信封; id 只取一个合法 JSON 值, 不能把后续 ,} 一起吞掉 */
    char id[48] = "null";
    const char *ip = json_find_key(body, "id");
    if (ip) {
        int n = 0;
        if (*ip == '"') {
            id[n++] = *ip++;
            while (*ip && *ip != '"' && n < (int)sizeof(id) - 3) {
                if (*ip == '\\' && ip[1]) id[n++] = *ip++;
                id[n++] = *ip++;
            }
            if (*ip == '"') id[n++] = *ip;
        } else {
            while (*ip && n < (int)sizeof(id) - 2 &&
                   (isdigit((unsigned char)*ip) || *ip == '-' || *ip == '+' ||
                    *ip == '.' || isalpha((unsigned char)*ip)))
                id[n++] = *ip++;
        }
        id[n] = 0;
        if (n == 0) snprintf(id, sizeof(id), "null");
    }
    snprintf(out, cap, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}", id, result);
}

void write_status_pub(char *out, int cap) { write_status(out, cap); }
void write_guide_pub(char *out, int cap) { write_guide(out, cap); }
void write_serial_ports_pub(char *out, int cap) { write_serial_ports(out, cap); }
void write_video_devices_pub(char *out, int cap) { write_video_devs(out, cap); }

void mcp_tool_list(char *out, int cap) {
    snprintf(out, cap,
        "{\"tools\":["
        "{\"name\":\"status\",\"desc\":\"压缩状态摘要: 链路/视频/键鼠/UDP/MCP\"},"
        "{\"name\":\"video_devices\",\"desc\":\"枚举采集卡/摄像头\"},"
        "{\"name\":\"serial_ports\",\"desc\":\"枚举串口 (ESP32)\"},"
        "{\"name\":\"connect\",\"desc\":\"连接 ESP32; args: channel(0串口/1本机/2 UDP),serial_port,serial_baud,udp_host,udp_port\"},"
        "{\"name\":\"discover\",\"desc\":\"自动发现 AI-KVM 板子 IP（mDNS/广播/扫网段）\"},"
        "{\"name\":\"disconnect\",\"desc\":\"断开链路\"},"        "{\"name\":\"reconnect_guide\",\"desc\":\"断线重连向导分步清单与卡点\"},"
        "{\"name\":\"set_grab\",\"desc\":\"开关键鼠接管; args: grabbed\"},"
        "{\"name\":\"send_mouse\",\"desc\":\"注入鼠标; dx,dy,buttons b0/b1/b2,wheel,absolute,x,y\"},"
        "{\"name\":\"send_key\",\"desc\":\"注入键盘; code,down,ctrl,shift,alt,meta\"},"
        "{\"name\":\"send_text\",\"desc\":\"按 ASCII 字符串批量敲键盘; args: text\"},"
        "{\"name\":\"snapshot\",\"desc\":\"把当前帧原分辨率存 BMP; args: path\"},"
        "{\"name\":\"logs\",\"desc\":\"最近日志; args: limit\"},"
        "{\"name\":\"config_get\",\"desc\":\"读取配置\"},"
        "{\"name\":\"config_set\",\"desc\":\"热更新配置子集\"}"
        "]}");
}
