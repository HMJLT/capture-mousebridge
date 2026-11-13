/* main.c - CaptureMouseBridge 入口 (Win32 GUI, 无控制台, 默认不占 TCP 端口)
 * 用法: CaptureMouseBridge.exe [--source N] [--serial COM3] [--baud 115200]
 *                               [--channel udp|serial|hid] [--udp-host IP]
 *                               [--lan-port N] [--lan]
 */
#include "common.h"
#include "capture.h"
#include "link.h"
#include "pipe_mcp.h"
#include "lan_mcp.h"
#include "gui.h"
#include "input_bridge.h"
#include "udp_link.h"
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#pragma comment(lib, "shell32.lib")

static void init_defaults(void) {
    ZeroMemory(&g_state, sizeof(g_state));
    g_state.video_source = -1;
    g_state.video_width = 1280;
    g_state.video_height = 720;
    g_state.fps_cap = 60;
    g_state.channel = CH_UDP;
    g_state.serial_baud = 115200;
    /* Leave the destination empty until discovery or explicit configuration. */
    g_state.udp_host[0] = 0;
    g_state.udp_port = 4210;
    g_state.udp_interval_ms = 4;
    g_state.capture_enabled = true;
    g_state.grabbed = false;
    g_state.auto_reconnect = true;
    g_state.reconnect_interval = 3;
    g_state.lan_enabled = false;
    g_state.lan_port = 47800;
    g_state.lan_state = 0;
    g_state.boot_time = now_sec();
}

static void parse_args(void) {
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return;
    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--source") == 0 && i + 1 < argc) {
            g_state.video_source = _wtoi(argv[++i]);
        } else if (wcscmp(argv[i], L"--serial") == 0 && i + 1 < argc) {
            char tmp[CMB_MAX_PATH] = "";
            WideCharToMultiByte(CP_ACP, 0, argv[++i], -1, tmp, sizeof(tmp), NULL, NULL);
            strncpy_s(g_state.serial_port, sizeof(g_state.serial_port), tmp, _TRUNCATE);
        } else if (wcscmp(argv[i], L"--baud") == 0 && i + 1 < argc) {
            g_state.serial_baud = _wtoi(argv[++i]);
        } else if (wcscmp(argv[i], L"--channel") == 0 && i + 1 < argc) {
            const wchar_t *c = argv[++i];
            if (wcscmp(c, L"hid") == 0) g_state.channel = CH_HID;
            else if (wcscmp(c, L"udp") == 0) g_state.channel = CH_UDP;
            else g_state.channel = CH_SERIAL;
        } else if (wcscmp(argv[i], L"--udp-host") == 0 && i + 1 < argc) {
            char tmp[64] = "";
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, tmp, sizeof(tmp), NULL, NULL);
            strncpy_s(g_state.udp_host, sizeof(g_state.udp_host), tmp, _TRUNCATE);
        } else if (wcscmp(argv[i], L"--udp-port") == 0 && i + 1 < argc) {
            g_state.udp_port = _wtoi(argv[++i]);
        } else if (wcscmp(argv[i], L"--udp-interval") == 0 && i + 1 < argc) {
            g_state.udp_interval_ms = _wtoi(argv[++i]);
        } else if (wcscmp(argv[i], L"--fps") == 0 && i + 1 < argc) {
            g_state.fps_cap = _wtoi(argv[++i]);
        } else if (wcscmp(argv[i], L"--lan-port") == 0 && i + 1 < argc) {
            g_state.lan_port = _wtoi(argv[++i]);
        } else if (wcscmp(argv[i], L"--lan") == 0) {
            g_state.lan_enabled = true;
        }
    }
    LocalFree(argv);
}

static DWORD WINAPI backend_thread(LPVOID arg) {
    (void)arg;
    while (true) {
        link_poll();
        Sleep(60);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmd, int nShow) {
    (void)hPrev;
    (void)lpCmd;

    init_defaults();
    parse_args();

    /* Media Foundation / COM 需要在使用前初始化 (采集枚举与预览都走 MF) */
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    snprintf(g_state.mcp_endpoint, sizeof(g_state.mcp_endpoint), "pipe:%s", pipe_mcp_name());

    SetProcessDPIAware();

    if (g_state.capture_enabled && g_state.video_source >= 0)
        capture_start(g_state.video_source, g_state.video_width, g_state.video_height, g_state.fps_cap);

    link_start_reconnect_timer();
    if (g_state.channel == CH_SERIAL && g_state.serial_port[0])
        link_connect();

    /* 启动就在后台找板子: 找到会写回 IP, 若当前是 UDP 通道会自动连上 */
    udp_auto_discover_async();

    pipe_mcp_start();
    if (g_state.lan_enabled) {
        if (!lan_mcp_start(g_state.lan_port))
            cmb_log("error", "局域网 MCP 未能启动");
    }

    CreateThread(NULL, 0, backend_thread, NULL, 0, NULL);
    cmb_log("ok", "采集卡桥 v%s 启动 · 本机 MCP: %s", CMB_VERSION, pipe_mcp_name());

    int rc = gui_run(hInst, nShow);

    lan_mcp_stop();
    pipe_mcp_stop();
    capture_stop();
    link_disconnect();
    return rc;
}
