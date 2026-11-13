/* udp_link.c - 面向 AI-KVM 板子 (ESP32-S3) 的 UDP 通道
 *
 * 协议与板子固件 16 字节报文逐字节对齐 (见 aikvm/protocol.py):
 *   0      设备 0x01 键盘 / 0x02 鼠标 / 0x03 控制
 *   1      修饰键位掩码 (键盘) 或 控制子命令
 *   2..7   键盘 HID usage, 6-key rollover
 *   8..9   鼠标 dx int16 LE
 *   10..11 鼠标 dy int16 LE
 *   12     鼠标按键掩码
 *   13     滚轮 int8
 *   14..15 序号 uint16 LE (0 = 不去重)
 *
 * 鼠标在定时线程里合并发送 (间隔默认 12ms), 键盘按下/松开立即发送。
 */
#include "udp_link.h"
#include "common.h"
#include "link.h"
#include "input_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

#define UDP_PORT_DEFAULT 4210
#define UDP_INTERVAL_DEFAULT 4
#define UDP_CTRL_PING   0x01
#define UDP_CTRL_SET_INTERVAL 0x04
#define UDP_CTRL_STATUS 0x06
#define UDP_CTRL_RELEASE 0x05

static SOCKET g_sock = INVALID_SOCKET;
static struct sockaddr_in g_dst;
static bool g_wsa = false;
static bool g_open = false;
static int  g_seq = 0;
static int  g_interval = UDP_INTERVAL_DEFAULT;
static HANDLE g_thread = NULL;
static HANDLE g_wake = NULL;
static volatile LONG g_run = 0;
static CRITICAL_SECTION g_lock;
static bool g_lock_init = false;

static int  g_mdx = 0, g_mdy = 0, g_mbtn = 0, g_mwheel = 0;
static bool g_mdirty = false;
static double g_last_tx = 0;
static char g_err[160] = "";

static void ensure_lock(void) {
    if (!g_lock_init) { InitializeCriticalSection(&g_lock); g_lock_init = true; }
}

static int next_seq(void) {
    g_seq = (g_seq + 1) & 0xFFFF;
    if (g_seq == 0) g_seq = 1;
    return g_seq;
}

static bool raw_send(const unsigned char *pkt, int len) {
    if (!g_open || g_sock == INVALID_SOCKET) return false;
    int rc = sendto(g_sock, (const char *)pkt, len, 0,
                    (const struct sockaddr *)&g_dst, sizeof(g_dst));
    if (rc <= 0) {
        snprintf(g_err, sizeof(g_err), "UDP 发送失败 (err %d)", WSAGetLastError());
        return false;
    }
    g_state.udp_tx++;
    return true;
}

/* ---------- 报文构造 ---------- */

static void pkt_head(unsigned char *p, int dev) {
    memset(p, 0, 16);
    p[0] = (unsigned char)dev;
}

static int send_keyboard(int mods, const unsigned char *keys, int nkeys) {
    unsigned char p[16];
    pkt_head(p, 0x01);
    p[1] = (unsigned char)mods;
    if (nkeys > 6) nkeys = 6;
    for (int i = 0; i < nkeys; i++) p[2 + i] = keys[i];
    *(unsigned short *)(p + 14) = (unsigned short)next_seq();
    return raw_send(p, 16) ? 1 : 0;
}

static int send_mouse_packet(int dx, int dy, int buttons, int wheel) {
    unsigned char p[16];
    pkt_head(p, 0x02);
    *(short *)(p + 8) = (short)dx;
    *(short *)(p + 10) = (short)dy;
    p[12] = (unsigned char)(buttons & 0x07);
    p[13] = (unsigned char)(wheel & 0xFF);
    *(unsigned short *)(p + 14) = (unsigned short)next_seq();
    return raw_send(p, 16) ? 1 : 0;
}

static int send_control(int cmd, int arg) {
    unsigned char p[16];
    pkt_head(p, 0x03);
    p[1] = (unsigned char)cmd;
    *(unsigned short *)(p + 2) = (unsigned short)arg;
    *(unsigned short *)(p + 14) = (unsigned short)next_seq();
    return raw_send(p, 16) ? 1 : 0;
}

/* ---------- 鼠标合并发送线程 ---------- */

static DWORD WINAPI udp_worker(LPVOID arg) {
    (void)arg;
    int idle = 0;
    int hb_div = 1000 / (g_interval > 0 ? g_interval : UDP_INTERVAL_DEFAULT);
    if (hb_div < 1) hb_div = 1;
    while (g_run) {
        int dx = 0, dy = 0, btn = 0, wh = 0;
        bool dirty = false;
        EnterCriticalSection(&g_lock);
        if (g_mdirty) {
            dx = g_mdx; dy = g_mdy; btn = g_mbtn; wh = g_mwheel;
            g_mdx = g_mdy = g_mwheel = 0;
            g_mdirty = false;
            dirty = true;
        }
        LeaveCriticalSection(&g_lock);

        if (dirty) {
            /* 单包位移限 ±127, 超出拆包, 不丢精度 */
            int rx = dx, ry = dy, w = wh;
            do {
                int sx = rx > 127 ? 127 : (rx < -127 ? -127 : rx);
                int sy = ry > 127 ? 127 : (ry < -127 ? -127 : ry);
                send_mouse_packet(sx, sy, btn, w);
                rx -= sx; ry -= sy;
                w = 0;
                if (rx == 0 && ry == 0) break;
                Sleep(1);
            } while (g_run);
            idle = 0;
        } else {
            idle++;
        }

        /* 每 1 秒轮询一次板子状态, 顺带刷新链路健康度 */
        if (idle > 0 && (idle % hb_div) == 0) {
            send_control(UDP_CTRL_STATUS, 0);
        }
        if (g_wake) {
            /* 有新的鼠标位移立刻唤醒发送, 不再等满一个节拍 */
            WaitForSingleObject(g_wake, (DWORD)(g_interval > 0 ? g_interval : UDP_INTERVAL_DEFAULT));
        } else {
            Sleep(g_interval > 0 ? g_interval : UDP_INTERVAL_DEFAULT);
        }
    }
    return 0;
}

/* ---------- 对外接口 ---------- */

bool udp_open(const char *host, int port, int interval_ms) {
    udp_close();
    ensure_lock();

    if (!g_wsa) {
        WSADATA wd;
        if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) {
            snprintf(g_err, sizeof(g_err), "WSAStartup 失败");
            return false;
        }
        g_wsa = true;
    }
    if (!host || !host[0]) {
        snprintf(g_err, sizeof(g_err), "请先填写板子 IP");
        return false;
    }
    if (port <= 0 || port > 65535) port = UDP_PORT_DEFAULT;

    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) {
        snprintf(g_err, sizeof(g_err), "创建 UDP socket 失败 (err %d)", WSAGetLastError());
        return false;
    }
    /* 收到 ICMP 端口不可达不要直接让 send 失败 */
    int off = 0;
    unsigned long bio = 1;
    WSAIoctl(g_sock, SIO_UDP_CONNRESET, &off, sizeof(off), NULL, 0, &bio, NULL, NULL);

    ZeroMemory(&g_dst, sizeof(g_dst));
    g_dst.sin_family = AF_INET;
    g_dst.sin_port = htons((unsigned short)port);
    if (InetPtonA(AF_INET, host, &g_dst.sin_addr) != 1) {
        snprintf(g_err, sizeof(g_err), "IP 格式不对: %s", host);
        closesocket(g_sock); g_sock = INVALID_SOCKET;
        return false;
    }
    /* 只接收回包, 不阻塞 */
    u_long nb = 1;
    ioctlsocket(g_sock, FIONBIO, &nb);

    g_interval = interval_ms > 0 ? interval_ms : UDP_INTERVAL_DEFAULT;
    if (g_interval < 2) g_interval = 2;
    if (g_interval > 200) g_interval = 200;
    g_open = true;
    g_run = 1;
    g_state.udp_tx = 0;
    g_state.udp_rx = 0;
    g_state.udp_hid = 0;
    g_state.udp_reply[0] = 0;
    g_state.udp_last_rx_at = 0;
    if (!g_wake) g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_thread = CreateThread(NULL, 0, udp_worker, NULL, 0, NULL);    if (!g_thread) {
        g_open = false; g_run = 0;
        closesocket(g_sock); g_sock = INVALID_SOCKET;
        snprintf(g_err, sizeof(g_err), "无法创建 UDP 发送线程");
        return false;
    }
    cmb_log("ok", "UDP 通道就绪 -> %s:%d (间隔 %dms)", host, port, g_interval);
    send_control(UDP_CTRL_STATUS, 0);
    /* 让固件的 HID 上报节奏跟上本机发送节奏 */
    send_control(UDP_CTRL_SET_INTERVAL, g_interval);
    return true;
}

void udp_close(void) {
    if (g_thread) {
        g_run = 0;
        if (g_wake) SetEvent(g_wake);
        WaitForSingleObject(g_thread, 1500);
        CloseHandle(g_thread); g_thread = NULL;
    }
    if (g_wake) { CloseHandle(g_wake); g_wake = NULL; }
    if (g_sock != INVALID_SOCKET) { closesocket(g_sock); g_sock = INVALID_SOCKET; }
    g_open = false;
}

bool udp_is_open(void) { return g_open; }

const char *udp_last_error(void) { return g_err[0] ? g_err : "UDP 未连接"; }

int udp_selected(void) { return g_open; }

bool udp_send_mouse(int dx, int dy, int buttons, int wheel) {
    if (!g_open) return false;
    ensure_lock();
    EnterCriticalSection(&g_lock);
    g_mdx += dx;
    g_mdy += dy;
    if (buttons >= 0) g_mbtn = buttons & 0x07;
    if (wheel) g_mwheel += wheel;
    g_mdirty = true;
    LeaveCriticalSection(&g_lock);
    if (g_wake) SetEvent(g_wake);
    return true;
}

bool udp_send_keyboard(int mods, const unsigned char *keys, int nkeys) {
    if (!g_open) return false;
    return send_keyboard(mods, keys, nkeys) != 0;
}

bool udp_send_release_all(void) {
    if (!g_open) return false;
    send_control(UDP_CTRL_RELEASE, 0);
    send_keyboard(0, NULL, 0);
    return true;
}

/* 收状态回包: 固件回的是 JSON 文本, 只挑几个关键字段, 省得引 JSON 解析 */
void udp_poll(void) {
    if (!g_open || g_sock == INVALID_SOCKET) return;

    for (int i = 0; i < 8; i++) {
        char buf[1024];
        struct sockaddr_in from;
        int flen = sizeof(from);
        int n = recvfrom(g_sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &flen);
        if (n <= 0) break;
        buf[n] = 0;
        g_state.udp_rx++;
        g_state.udp_last_rx_at = now_sec();

        if (strstr(buf, "\"hid_mounted\"")) {
            const char *p = strstr(buf, "\"hid_mounted\"");
            p = strchr(p, ':');
            if (p) {
                p++;
                while (*p == ' ') p++;
                g_state.udp_hid = (strncmp(p, "true", 4) == 0) ? 1 : 0;
            }
        }
        if (strstr(buf, "\"ip\"")) {
            const char *p = strstr(buf, "\"ip\"");
            p = strchr(p, ':');
            if (p) {
                p++;
                while (*p == ' ' || *p == '"') p++;
                int k = 0;
                while (*p && *p != '"' && k < (int)sizeof(g_state.udp_reply) - 1)
                    g_state.udp_reply[k++] = *p++;
                g_state.udp_reply[k] = 0;
            }
        }
        if (g_state.link_state != LINK_CONNECTED) {
            g_state.link_state = LINK_CONNECTED;
            g_state.last_ok_at = now_sec();
            cmb_log("ok", "板子已应答 (UDP %s)", g_state.udp_reply[0] ? g_state.udp_reply : "?");
        }
    }

    /* 超过 6 秒没有任何回包 -> 链路判定断开, 交给自动重连 */
    if (g_state.link_state == LINK_CONNECTED &&
        g_state.udp_last_rx_at > 0 &&
        now_sec() - g_state.udp_last_rx_at > 6.0) {
        input_release_all();
        g_state.link_state = LINK_DISCONNECTED;
        cmb_log("warn", "板子 6 秒无回包, UDP 链路断开");
    }
}

/* ================= 自动发现 ================= */

static volatile LONG g_disc_hit = 0;
static volatile LONG g_discovering = 0;

/* 从状态 JSON 里抠 hid_mounted 与 phase, 不引 JSON 库 */
static void parse_status(const char *json, int *hid, char *phase, int phasecap) {
    if (hid) {
        const char *p = strstr(json, "\"hid_mounted\"");
        if (p) {
            p = strchr(p, ':');
            if (p) { p++; while (*p == ' ') p++; *hid = (strncmp(p, "true", 4) == 0) ? 1 : 0; }
        }
    }
    if (phase && phasecap > 0) {
        const char *p = strstr(json, "\"phase\"");
        if (p) {
            p = strchr(p, ':');
            if (p) {
                p++; while (*p == ' ' || *p == '"') p++;
                int k = 0;
                while (*p && *p != '"' && k < phasecap - 1) phase[k++] = *p++;
                phase[k] = 0;
            }
        }
    }
}

static int g_probe_seq = 0;

/* 发现/探测用独立的序号序列, 不干扰正常键鼠报文的连续性 */
static void make_status_packet(unsigned char *p) {
    memset(p, 0, 16);
    p[0] = 0x03;
    p[1] = UDP_CTRL_STATUS;
    g_probe_seq = (g_probe_seq + 1) & 0xFFFF;
    if (g_probe_seq == 0) g_probe_seq = 1;
    *(unsigned short *)(p + 14) = (unsigned short)g_probe_seq;
}

static void prepare_probe_socket(SOCKET s) {
    int off = 0;
    unsigned long bio = 1;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), NULL, 0, &bio, NULL, NULL);
}

/* 向单个地址发 STATUS, 拿到 JSON 就返回 true (会重发, 冷 ARP 首包常丢) */
static bool probe_one(const char *host, int port, char *json, int cap, int budget_ms) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    prepare_probe_socket(s);
    struct sockaddr_in to;
    ZeroMemory(&to, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons((unsigned short)port);
    if (InetPtonA(AF_INET, host, &to.sin_addr) != 1) { closesocket(s); return false; }

    DWORD start = GetTickCount();
    DWORD last_send = 0;
    bool hit = false;
    while (GetTickCount() - start < (DWORD)budget_ms) {
        DWORD now = GetTickCount();
        if (now - last_send > 300) {
            unsigned char pkt[16];
            make_status_packet(pkt);
            sendto(s, (const char *)pkt, 16, 0, (struct sockaddr *)&to, sizeof(to));
            last_send = now;
        }
        fd_set rd;
        FD_ZERO(&rd); FD_SET(s, &rd);
        struct timeval tv = { 0, 80 * 1000 };
        int rc = select(0, &rd, NULL, NULL, &tv);
        if (rc > 0) {
            char buf[1024];
            int n = recv(s, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = 0;
                if (strchr(buf, '{')) {
                    strncpy(json, buf, cap - 1);
                    json[cap - 1] = 0;
                    hit = true;
                    break;
                }
            }
        }
    }
    closesocket(s);
    return hit;
}

/* 本机各 IPv4 所在的 /24 网段 */
static int local_subnets(char out[][20], int maxn) {
    ULONG size = 16384;
    IP_ADAPTER_ADDRESSES *aa = (IP_ADAPTER_ADDRESSES *)malloc(size);
    if (!aa) return 0;
    DWORD rc = GetAdaptersAddresses(AF_INET,
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        NULL, aa, &size);
    if (rc != NO_ERROR) {
        free(aa);
        aa = (IP_ADAPTER_ADDRESSES *)malloc(size);
        if (!aa) return 0;
        rc = GetAdaptersAddresses(AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            NULL, aa, &size);
        if (rc != NO_ERROR) { free(aa); return 0; }
    }

    int n = 0;
    for (IP_ADAPTER_ADDRESSES *a = aa; a && n < maxn; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u && n < maxn; u = u->Next) {
            struct sockaddr_in *sa = (struct sockaddr_in *)u->Address.lpSockaddr;
            if (!sa || sa->sin_family != AF_INET) continue;
            unsigned char *b = (unsigned char *)&sa->sin_addr;
            bool priv = (b[0] == 10) ||
                        (b[0] == 172 && b[1] >= 16 && b[1] <= 31) ||
                        (b[0] == 192 && b[1] == 168);
            if (!priv) continue;
            char net[20];
            snprintf(net, sizeof(net), "%d.%d.%d", b[0], b[1], b[2]);
            bool dup = false;
            for (int i = 0; i < n; i++) if (strcmp(out[i], net) == 0) { dup = true; break; }
            if (!dup) strncpy(out[n++], net, 20);
        }
    }
    free(aa);
    return n;
}

/* 扫本机各 /24: 连发 STATUS, 再统一收包 */
static bool sweep_subnets(int port, char *ip_out, int cap, int *hid, char *json_out, int json_cap) {
    char nets[8][20];
    int nn = local_subnets(nets, 8);
    if (nn <= 0) return false;

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    prepare_probe_socket(s);

    struct sockaddr_in to;
    ZeroMemory(&to, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons((unsigned short)port);

    for (int sweep = 0; sweep < 2; sweep++) {
        for (int i = 0; i < nn; i++) {
            for (int last = 1; last < 255; last++) {
                char host[20];
                snprintf(host, sizeof(host), "%s.%d", nets[i], last);
                if (InetPtonA(AF_INET, host, &to.sin_addr) != 1) continue;
                unsigned char pkt[16];
                make_status_packet(pkt);
                sendto(s, (const char *)pkt, 16, 0, (struct sockaddr *)&to, sizeof(to));
            }
        }
        Sleep(400);
    }

    bool hit = false;
    DWORD start = GetTickCount();
    while (GetTickCount() - start < 2600) {
        fd_set rd;
        FD_ZERO(&rd); FD_SET(s, &rd);
        struct timeval tv = { 0, 100 * 1000 };
        int rc = select(0, &rd, NULL, NULL, &tv);
        if (rc <= 0) continue;
        char buf[1024];
        struct sockaddr_in from;
        int flen = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &flen);
        if (n <= 0) continue;
        buf[n] = 0;
        if (!strchr(buf, '{')) continue;
        char ip[64]; InetNtopA(AF_INET, &from.sin_addr, ip, sizeof(ip));
        strncpy(ip_out, ip, cap - 1);
        ip_out[cap - 1] = 0;
        strncpy(json_out, buf, json_cap - 1);
        json_out[json_cap - 1] = 0;
        parse_status(buf, hid, NULL, 0);
        hit = true;
        break;
    }
    closesocket(s);
    return hit;
}

bool udp_discover_now(char *ip_out, int cap, int *hid_out, char *json_out, int json_cap) {
    int port = g_state.udp_port > 0 ? g_state.udp_port : UDP_PORT_DEFAULT;
    char json[1024] = "";
    bool hit = false;

    if (!g_wsa) {
        WSADATA wd;
        if (WSAStartup(MAKEWORD(2, 2), &wd) == 0) g_wsa = true;
    }

    /* 1) mDNS: 板子注册了 ai-kvm.local */
    char ip[64] = "";
    struct addrinfo hints, *res = NULL;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    for (int i = 0; i < 2 && !hit; i++) {
        if (getaddrinfo("ai-kvm.local", NULL, &hints, &res) == 0 && res) {
            struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
            InetNtopA(AF_INET, &sa->sin_addr, ip, sizeof(ip));
            freeaddrinfo(res);
            res = NULL;
            if (probe_one(ip, port, json, sizeof(json), 1200)) hit = true;
        }
        if (!hit) Sleep(200);
    }
    if (hit) cmb_log("ok", "自动发现: mDNS ai-kvm.local -> %s", ip);

    /* 2) 同网段广播 */
    if (!hit) {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s != INVALID_SOCKET) {
            prepare_probe_socket(s);
            BOOL on = TRUE;
            setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof(on));
            struct sockaddr_in to;
            ZeroMemory(&to, sizeof(to));
            to.sin_family = AF_INET;
            to.sin_port = htons((unsigned short)port);
            to.sin_addr.s_addr = INADDR_BROADCAST;
            unsigned char pkt[16];
            make_status_packet(pkt);
            sendto(s, (const char *)pkt, 16, 0, (struct sockaddr *)&to, sizeof(to));
            DWORD start = GetTickCount();
            while (GetTickCount() - start < 1300) {
                fd_set rd;
                FD_ZERO(&rd); FD_SET(s, &rd);
                struct timeval tv = { 0, 100 * 1000 };
                if (select(0, &rd, NULL, NULL, &tv) <= 0) continue;
                char buf[1024];
                struct sockaddr_in from;
                int flen = sizeof(from);
                int n = recvfrom(s, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &flen);
                if (n <= 0) continue;
                buf[n] = 0;
                if (!strchr(buf, '{')) continue;
                InetNtopA(AF_INET, &from.sin_addr, ip, sizeof(ip));
                strncpy(json, buf, sizeof(json) - 1);
                hit = true;
                break;
            }
            closesocket(s);
            if (hit) cmb_log("ok", "自动发现: 广播找到板子 %s", ip);
        }
    }

    /* 3) 扫本机各 /24 */
    if (!hit) {
        hit = sweep_subnets(port, ip, sizeof(ip), hid_out, json, sizeof(json));
        if (hit) cmb_log("ok", "自动发现: 扫网段找到板子 %s", ip);
    }

    if (hit) {
        if (ip_out) { strncpy(ip_out, ip, cap - 1); ip_out[cap - 1] = 0; }
        if (json_out) { strncpy(json_out, json, json_cap - 1); json_out[json_cap - 1] = 0; }
        if (hid_out) { int h = 0; parse_status(json, &h, NULL, 0); *hid_out = h; }
    }
    return hit;
}

static DWORD WINAPI discover_worker(LPVOID arg) {
    (void)arg;
    char ip[64] = "", json[1024] = "";
    int hid = 0;
    if (udp_discover_now(ip, sizeof(ip), &hid, json, sizeof(json))) {
        strncpy(g_state.udp_host, ip, sizeof(g_state.udp_host) - 1);
        g_state.udp_hid = hid;
        char phase[32] = "";
        parse_status(json, &hid, phase, sizeof(phase));
        if (phase[0]) snprintf(g_state.udp_reply, sizeof(g_state.udp_reply), "%s", phase);
        g_disc_hit = 1;
    } else {
        cmb_log("warn", "自动发现没找到板子（检查板子是否上电并和电脑在同一个 Wi-Fi）");
    }
    g_discovering = 0;
    return 0;
}

void udp_auto_discover_async(void) {
    if (InterlockedCompareExchange(&g_discovering, 1, 0) != 0) return;
    g_state.udp_discovering = true;
    CreateThread(NULL, 0, discover_worker, NULL, 0, NULL);
}

bool udp_take_discovery(void) {
    g_state.udp_discovering = (g_discovering != 0);
    return InterlockedExchange(&g_disc_hit, 0) != 0;
}
