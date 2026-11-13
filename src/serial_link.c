/* serial_link.c - Win32 串口: 枚举 / 连接 / 收发 */
#include "serial_link.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static HANDLE g_com = INVALID_HANDLE_VALUE;
static char   g_rx_buf[1024];
static int    g_rx_len = 0;
static char   g_err[256] = "";

int serial_enumerate(serial_port_t *out, int max) {
    int count = 0;
    char path[CMB_MAX_PATH];
    HANDLE h;
    DCB dcb;
    COMMCONFIG cc;
    DWORD size = sizeof(cc);

    for (int i = 1; i <= CMB_MAX_PORTS; i++) {
        if (count >= max) break;
        snprintf(path, sizeof(path), "COM%d", i);
        h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                        OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;

        ZeroMemory(&dcb, sizeof(dcb));
        dcb.DCBlength = sizeof(dcb);
        bool ok = GetCommState(h, &dcb);

        ZeroMemory(&cc, sizeof(cc));
        cc.dwSize = sizeof(cc);
        DWORD guidCount = 0;
        /* 读友好名: SetupDi 成本高, 这里用注册表枚举设备友好名 */
        char friendly[256] = "";
        HKEY hk;
        char sub[128];
        snprintf(sub, sizeof(sub), "HARDWARE\\DEVICEMAP\\SERIALCOMM");
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, sub, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
            for (DWORD idx = 0; ; idx++) {
                char valName[128]; DWORD vnSize = sizeof(valName);
                char valData[128]; DWORD vdSize = sizeof(valData);
                DWORD type;
                if (RegEnumValueA(hk, idx, valName, &vnSize, NULL, &type,
                                  (LPBYTE)valData, &vdSize) != ERROR_SUCCESS) break;
                if (_stricmp(valData, path) == 0) {
                    strncpy(friendly, valName, sizeof(friendly) - 1);
                    break;
                }
            }
            RegCloseKey(hk);
        }

        if (ok && count < max) {
            strncpy(out[count].port, path, sizeof(out[count].port) - 1);
            strncpy(out[count].description, friendly[0] ? friendly : "Serial Port",
                    sizeof(out[count].description) - 1);
            out[count].vid = 0;
            out[count].pid = 0;
            out[count].has_hwid = false;
            count++;
        }
        CloseHandle(h);
    }
    return count;
}

bool serial_open(const char *port, int baud) {
    serial_close();
    g_err[0] = 0;
    if (!port || !port[0]) { strcpy_s(g_err, sizeof(g_err), "未选择串口"); return false; }

    char full[CMB_MAX_PATH];
    /* Win32 对 COM10+ 需要 \\.\ 前缀 */
    if (strncmp(port, "\\\\.\\", 4) != 0)
        snprintf(full, sizeof(full), "\\\\.\\%s", port);
    else
        strncpy(full, port, sizeof(full) - 1);

    g_com = CreateFileA(full, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (g_com == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        snprintf(g_err, sizeof(g_err), "打开失败 (err %lu)", e);
        return false;
    }

    DCB dcb; ZeroMemory(&dcb, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(g_com, &dcb)) {
        snprintf(g_err, sizeof(g_err), "GetCommState 失败");
        CloseHandle(g_com); g_com = INVALID_HANDLE_VALUE;
        return false;
    }
    dcb.BaudRate = baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    if (!SetCommState(g_com, &dcb)) {
        snprintf(g_err, sizeof(g_err), "SetCommState 失败");
        CloseHandle(g_com); g_com = INVALID_HANDLE_VALUE;
        return false;
    }
    COMMTIMEOUTS to;
    to.ReadIntervalTimeout = 20;
    to.ReadTotalTimeoutConstant = 20;
    to.ReadTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant = 100;
    to.WriteTotalTimeoutMultiplier = 0;
    SetCommTimeouts(g_com, &to);
    PurgeComm(g_com, PURGE_RXCLEAR | PURGE_TXCLEAR);

    g_rx_len = 0;
    return true;
}

void serial_close(void) {
    if (g_com != INVALID_HANDLE_VALUE) {
        CloseHandle(g_com);
        g_com = INVALID_HANDLE_VALUE;
    }
    g_rx_len = 0;
}

bool serial_is_open(void) { return g_com != INVALID_HANDLE_VALUE; }

const char *serial_last_error(void) { return g_err; }

bool serial_send_line(const char *line) {
    if (g_com == INVALID_HANDLE_VALUE) return false;
    char buf[CMB_MAX_LINE];
    snprintf(buf, sizeof(buf), "%s\n", line);
    DWORD written = 0;
    BOOL ok = WriteFile(g_com, buf, (DWORD)strlen(buf), &written, NULL);
    if (!ok || written == 0) {
        snprintf(g_err, sizeof(g_err), "WriteFile 失败 err %lu", GetLastError());
        return false;
    }
    return true;
}

static void handle_rx_line(char *line) {
    if (!line[0]) return;
    g_state.last_ok_at = now_sec();
    if (strncmp(line, "P,pong", 6) == 0) {
        cmb_log("debug", "收到 ESP32 心跳 pong");
    } else {
        cmb_log("rx", "ESP32: %.160s", line);
    }
}

void serial_poll_rx(void) {
    if (g_com == INVALID_HANDLE_VALUE) return;
    char chunk[256]; DWORD read = 0;
    if (!ReadFile(g_com, chunk, sizeof(chunk), &read, NULL)) return;
    if (read == 0) return;
    for (DWORD i = 0; i < read; i++) {
        if (g_rx_len >= (int)sizeof(g_rx_buf) - 1) g_rx_len = 0;
        if (chunk[i] == '\n' || chunk[i] == '\r') {
            g_rx_buf[g_rx_len] = 0;
            handle_rx_line(g_rx_buf);
            g_rx_len = 0;
        } else {
            g_rx_buf[g_rx_len++] = chunk[i];
        }
    }
}
