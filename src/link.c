/* link.c - 链路状态机 + 自动重连 */
#include "link.h"
#include "serial_link.h"
#include "udp_link.h"
#include "input_bridge.h"
#include <stdio.h>
#include <string.h>

static double g_last_attempt = 0;

bool link_connect(void) {
    g_state.link_state = LINK_CONNECTING;
    g_state.link_error[0] = 0;

    if (g_state.channel == CH_SERIAL) {
        if (serial_open(g_state.serial_port, g_state.serial_baud)) {
            g_state.link_state = LINK_CONNECTED;
            strncpy(g_state.link_device, g_state.serial_port,
                    sizeof(g_state.link_device) - 1);
            g_state.link_baud = g_state.serial_baud;
            g_state.last_ok_at = now_sec();
            g_state.reconnect_attempts = 0;
            cmb_log("ok", "串口已连接 %s @ %d", g_state.serial_port, g_state.serial_baud);
            return true;
        }
        strncpy(g_state.link_error, serial_last_error(),
                sizeof(g_state.link_error) - 1);
        g_state.link_state = LINK_FAILED;
        cmb_log("error", "串口连接失败: %s", g_state.link_error);
        return false;
    }

    if (g_state.channel == CH_UDP) {
        if (udp_open(g_state.udp_host, g_state.udp_port, g_state.udp_interval_ms)) {
            g_state.link_state = LINK_CONNECTED;   /* 首包已发, 等状态回包确认 */
            snprintf(g_state.link_device, sizeof(g_state.link_device),
                     "udp:%s:%d", g_state.udp_host, g_state.udp_port);
            g_state.link_baud = 0;
            g_state.last_ok_at = now_sec();
            g_state.reconnect_attempts = 0;
            cmb_log("ok", "UDP 通道已连接 %s:%d", g_state.udp_host, g_state.udp_port);
            return true;
        }
        strncpy(g_state.link_error, udp_last_error(), sizeof(g_state.link_error) - 1);
        g_state.link_state = LINK_FAILED;
        cmb_log("error", "UDP 连接失败: %s", g_state.link_error);
        return false;
    }

    if (g_state.channel == CH_HID) {
        g_state.link_state = LINK_CONNECTED;
        strncpy(g_state.link_device, "local-hid", sizeof(g_state.link_device) - 1);
        g_state.link_baud = 0;
        g_state.last_ok_at = now_sec();
        cmb_log("ok", "本地 HID 模式就绪");
        return true;
    }

    /* CH_WS 预留 */
    strcpy_s(g_state.link_error, sizeof(g_state.link_error), "未知通道");
    g_state.link_state = LINK_FAILED;
    return false;
}

void link_disconnect(void) {
    input_release_all();
    serial_close();
    udp_close();
    g_state.link_state = LINK_IDLE;
    g_state.link_device[0] = 0;
    g_state.link_error[0] = 0;
    cmb_log("warn", "链路已断开");
}

void link_poll(void) {
    serial_poll_rx();
    if (g_state.channel == CH_UDP) udp_poll();

    /* 后台自动发现拿到结果 -> 自动切 UDP 并连上, 不用用户手点 */
    if (udp_take_discovery()) {
        bool idle = (g_state.link_state == LINK_IDLE ||
                     g_state.link_state == LINK_FAILED ||
                     g_state.link_state == LINK_DISCONNECTED);
        /* 串口没配又没连上时, 直接把通道切到 UDP, 做到"打开就能用" */
        if (idle && g_state.channel == CH_SERIAL && g_state.serial_port[0] == 0)
            g_state.channel = CH_UDP;
        if (idle && g_state.channel == CH_UDP) {
            cmb_log("ok", "自动发现到板子 %s，正在建立 UDP 链路", g_state.udp_host);
            link_connect();
        }
        cmb_log("ok", "已记录板子地址 %s:%d", g_state.udp_host, g_state.udp_port);
    }
    if (g_state.channel == CH_SERIAL && g_state.link_state == LINK_CONNECTED) {
        if (!serial_is_open()) {
            input_release_all();
            g_state.link_state = LINK_DISCONNECTED;
            cmb_log("warn", "串口链路丢失");
        }
    }
    if (g_state.channel == CH_UDP && g_state.link_state == LINK_CONNECTED) {
        if (!udp_is_open()) {
            input_release_all();
            g_state.link_state = LINK_DISCONNECTED;
            cmb_log("warn", "UDP 通道已关闭");
        }
    }

    /* 自动重连 */
    if (g_state.auto_reconnect &&
        (g_state.link_state == LINK_DISCONNECTED || g_state.link_state == LINK_FAILED)) {
        double t = now_sec();
        if (t - g_last_attempt >= g_state.reconnect_interval) {
            g_last_attempt = t;
            g_state.reconnect_attempts++;
            cmb_log("warn", "第 %d 次尝试重连 ESP32", g_state.reconnect_attempts);
            link_connect();
        }
    }
}

void link_start_reconnect_timer(void) { g_last_attempt = 0; }
