/* lan_mcp.c - 可选局域网 TCP MCP: 每行一条 JSON-RPC, 回包同样以 \n 结尾
 * 仅在设置里打开时监听, 默认关闭; 本机请用命名管道 (pipe_mcp.c)。
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include "lan_mcp.h"
#include "mcp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")

static SOCKET g_listen = INVALID_SOCKET;
static HANDLE g_thread = NULL;
static volatile LONG g_stop = 0;
static volatile LONG g_clients = 0;
static bool g_wsa = false;

#define LAN_MCP_MAX_REQUEST 8192
#define LAN_MCP_MAX_CLIENTS 8
#define LAN_MCP_SOCKET_TIMEOUT_MS 1000

static const char *g_request_too_large =
    "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32600,\"message\":\"request too large\"}}\n";

static bool send_all(SOCKET s, const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(s, buf + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

static DWORD WINAPI client_worker(LPVOID arg) {
    SOCKET s = (SOCKET)(INT_PTR)arg;
    g_state.lan_clients = (int)InterlockedCompareExchange(&g_clients, 0, 0);
    cmb_log("mcp", "局域网客户端接入");

    char req[LAN_MCP_MAX_REQUEST + 1];
    char out[CMB_MAX_JSON];
    int total = 0;
    bool discarding = false;

    while (!g_stop) {
        char ch = 0;
        int n = recv(s, &ch, 1, 0);
        if (n == 0) break;
        if (n < 0) {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) continue;
            break;
        }
        if (ch == '\n') {
            if (!discarding && total > 0) {
                req[total] = 0;
                mcp_handle(req, out, sizeof(out));
                int len = (int)strlen(out);
                if (!send_all(s, out, len) || !send_all(s, "\n", 1)) break;
            } else if (discarding) {
                if (!send_all(s, g_request_too_large,
                              (int)strlen(g_request_too_large))) break;
            }
            total = 0;
            discarding = false;
        } else if (ch != '\r') {
            if (!discarding) {
                if (total < (int)sizeof(req) - 1) req[total++] = ch;
                else discarding = true;
            }
        }
    }
    closesocket(s);
    InterlockedDecrement(&g_clients);
    g_state.lan_clients = (int)InterlockedCompareExchange(&g_clients, 0, 0);
    cmb_log("mcp", "局域网客户端断开");
    return 0;
}

static DWORD WINAPI accept_worker(LPVOID arg) {
    (void)arg;
    while (!g_stop) {
        SOCKET c = accept(g_listen, NULL, NULL);
        if (c == INVALID_SOCKET) {
            if (g_stop) break;
            Sleep(200);
            continue;
        }
        if (InterlockedCompareExchange(&g_clients, 0, 0) >= LAN_MCP_MAX_CLIENTS) {
            closesocket(c);
            continue;
        }
        InterlockedIncrement(&g_clients);
        g_state.lan_clients = (int)InterlockedCompareExchange(&g_clients, 0, 0);
        int timeout = LAN_MCP_SOCKET_TIMEOUT_MS;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
        u_long nb = 0;
        ioctlsocket(c, FIONBIO, &nb);
        HANDLE worker = CreateThread(NULL, 0, client_worker, (LPVOID)(INT_PTR)c, 0, NULL);
        if (!worker) {
            closesocket(c);
            InterlockedDecrement(&g_clients);
            g_state.lan_clients = (int)InterlockedCompareExchange(&g_clients, 0, 0);
        } else {
            CloseHandle(worker);
        }
    }
    return 0;
}

bool lan_mcp_start(int port) {
    if (g_listen != INVALID_SOCKET) return true;
    if (port < 1 || port > 65535) {
        strncpy_s(g_state.lan_error, sizeof(g_state.lan_error), "端口需在 1..65535", _TRUNCATE);
        g_state.lan_state = 2;
        return false;
    }
    if (!g_wsa) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            strncpy_s(g_state.lan_error, sizeof(g_state.lan_error), "WSAStartup 失败", _TRUNCATE);
            g_state.lan_state = 2;
            return false;
        }
        g_wsa = true;
    }

    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET) {
        strncpy_s(g_state.lan_error, sizeof(g_state.lan_error), "创建套接字失败", _TRUNCATE);
        g_state.lan_state = 2;
        return false;
    }

    BOOL reuse = TRUE;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

    struct sockaddr_in addr;
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    InetPtonA(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (bind(g_listen, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        snprintf(g_state.lan_error, sizeof(g_state.lan_error),
                 "端口 %d 被占用 (err %d)", port, WSAGetLastError());
        closesocket(g_listen); g_listen = INVALID_SOCKET;
        g_state.lan_state = 2;
        cmb_log("error", "局域网 MCP 启动失败: %s", g_state.lan_error);
        return false;
    }
    if (listen(g_listen, 8) == SOCKET_ERROR) {
        strncpy_s(g_state.lan_error, sizeof(g_state.lan_error), "listen 失败", _TRUNCATE);
        closesocket(g_listen); g_listen = INVALID_SOCKET;
        g_state.lan_state = 2;
        return false;
    }

    g_stop = 0;
    g_clients = 0;
    g_state.lan_clients = 0;
    g_state.lan_port = port;
    g_state.lan_state = 1;
    g_state.lan_enabled = true;
    g_state.lan_error[0] = 0;
    snprintf(g_state.lan_addr, sizeof(g_state.lan_addr), "127.0.0.1:%d", port);
    g_thread = CreateThread(NULL, 0, accept_worker, NULL, 0, NULL);
    cmb_log("ok", "局域网 MCP 已监听 %s", g_state.lan_addr);
    return true;
}

void lan_mcp_stop(void) {
    if (g_listen == INVALID_SOCKET) return;
    g_stop = 1;
    closesocket(g_listen);
    g_listen = INVALID_SOCKET;
    if (g_thread) { WaitForSingleObject(g_thread, 400); CloseHandle(g_thread); g_thread = NULL; }
    DWORD deadline = GetTickCount() + 1500;
    while (InterlockedCompareExchange(&g_clients, 0, 0) > 0 &&
           GetTickCount() < deadline) {
        Sleep(20);
    }
    g_state.lan_state = 0;
    g_state.lan_enabled = false;
    g_state.lan_clients = (int)InterlockedCompareExchange(&g_clients, 0, 0);
    g_state.lan_addr[0] = 0;
    cmb_log("warn", "局域网 MCP 已关闭");
}

bool lan_mcp_running(void) { return g_listen != INVALID_SOCKET; }
