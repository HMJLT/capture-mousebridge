/* pipe_mcp.c - Windows 命名管道上的 MCP, 不监听 TCP 端口
 * 协议: 每条请求以 \n 结尾的 JSON; 响应同样以 \n 结尾。
 * Agent 通过 CreateFile("\\\\.\\pipe\\cmb-mcp") 连接, 写入 JSON-RPC, 读取回包。
 */
#include "pipe_mcp.h"
#include "mcp.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PIPE_NAME "\\\\.\\pipe\\cmb-mcp"

static HANDLE g_pipe = INVALID_HANDLE_VALUE;
static HANDLE g_thread = NULL;
static volatile LONG g_stop = 0;

const char *pipe_mcp_name(void) { return PIPE_NAME; }

static void handle_client(HANDLE pipe) {
    char req[CMB_MAX_JSON];
    char out[CMB_MAX_JSON];
    DWORD total = 0;

    while (!g_stop) {
        DWORD read = 0;
        BOOL ok = ReadFile(pipe, req + total, 1, &read, NULL);
        if (!ok || read == 0) break;
        if (req[total] == '\n') {
            req[total] = 0;
            mcp_handle(req, out, sizeof(out));
            DWORD written = 0;
            char *nl = "\n";
            WriteFile(pipe, out, (DWORD)strlen(out), &written, NULL);
            WriteFile(pipe, nl, 1, &written, NULL);
            total = 0;
        } else {
            total++;
            if (total >= (int)sizeof(req) - 1) total = 0;
        }
    }
    FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe);
}

static DWORD WINAPI client_thread(LPVOID arg) {
    HANDLE pipe = (HANDLE)arg;
    handle_client(pipe);
    CloseHandle(pipe);
    return 0;
}

static DWORD WINAPI pipe_worker(LPVOID arg) {
    (void)arg;
    while (!g_stop) {
        HANDLE pipe = CreateNamedPipeA(
            PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            CMB_MAX_JSON, CMB_MAX_JSON, 0, NULL);
        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(500);
            continue;
        }
        BOOL connected = ConnectNamedPipe(pipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected) {
            CloseHandle(pipe);
            continue;
        }
        CreateThread(NULL, 0, client_thread, pipe, 0, NULL);
    }
    return 0;
}

void pipe_mcp_start(void) {
    g_stop = 0;
    g_thread = CreateThread(NULL, 0, pipe_worker, NULL, 0, NULL);
    cmb_log("ok", "MCP 管道就绪: %s", PIPE_NAME);
}

void pipe_mcp_stop(void) {
    g_stop = 1;
    if (g_pipe != INVALID_HANDLE_VALUE) { CloseHandle(g_pipe); g_pipe = INVALID_HANDLE_VALUE; }
    if (g_thread) { WaitForSingleObject(g_thread, 300); CloseHandle(g_thread); g_thread = NULL; }
}
