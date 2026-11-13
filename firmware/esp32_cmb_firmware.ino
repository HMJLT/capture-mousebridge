/*
 * CaptureMouseBridge - ESP32 HID 固件示例
 * 平台: ESP32-S2 / S3 / C3 (支持 USB HID 的型号), Arduino 框架
 * 功能: 接收上位机 C 后端的行式协议, 模拟 USB 鼠标/键盘; 回心跳 pong。
 *
 * 协议 (与上位机 server 协议表一致, 行尾 \n):
 *   M,dx,dy,l,r,s,w     鼠标相对移动; l/r/s 左键/右键/中键 0/1; w 滚轮
 *   M,x,y,1             鼠标绝对定位 (0..1000)
 *   K,down,code,mods    键盘按下; code=Windows VK, mods=位掩码 ctrl/shift/alt/meta
 *   K,up,code,mods      键盘松开
 *   P,ping              心跳 -> 固件回 P,pong
 *
 * 接线: ESP32 USB 直接连被控电脑; 上位机通过串口(或 USB CDC)把事件发过来。
 * 注意: 若 ESP32 用原生 USB HID, 需在 Arduino IDE 勾选 "USB Mode: USB-OTG / HID"。
 */

#include <Arduino.h>
#include <HID.h>
#include <stdlib.h>
#include <string.h>

// Windows VK 常量 (与 WinUser.h 对齐, 避免依赖 Windows 头)
#define VK_RETURN   0x0D
#define VK_ESCAPE   0x1B
#define VK_BACK     0x08
#define VK_TAB      0x09
#define VK_SPACE    0x20
#define VK_SHIFT    0x10
#define VK_CONTROL  0x11
#define VK_MENU     0x12
#define VK_LSHIFT   0xA0
#define VK_RSHIFT   0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU    0xA4
#define VK_RMENU    0xA5
#define VK_UP       0x26
#define VK_DOWN     0x28
#define VK_LEFT     0x25
#define VK_RIGHT    0x27
#define VK_F1       0x70
#define VK_F2       0x71
#define VK_F3       0x72
#define VK_F4       0x73
#define VK_F5       0x74
#define VK_F6       0x75
#define VK_F7       0x76
#define VK_F8       0x77
#define VK_F9       0x78
#define VK_F10      0x79
#define VK_F11      0x7A
#define VK_F12      0x7B

static const uint8_t REPORT_ID_MOUSE = 1;
static const uint8_t REPORT_ID_KEYBOARD = 2;

// 极简 HID 描述符: 鼠标 + 键盘
static const uint8_t hidReportDescriptor[] = {
    // Mouse
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01,
    0x85, REPORT_ID_MOUSE,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x03, 0x81, 0x02,
    0x75, 0x05, 0x95, 0x01, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
    0xC0,
    // Keyboard
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
    0x85, REPORT_ID_KEYBOARD,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x03,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07,
    0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0
};

class CMBHID : public HID_ {
public:
    void begin() { HID_.begin(); }
    void mouseMove(int8_t dx, int8_t dy, uint8_t buttons, int8_t wheel) {
        uint8_t rep[5] = {REPORT_ID_MOUSE, buttons, (uint8_t)dx, (uint8_t)dy, (uint8_t)wheel};
        sendReport(rep, sizeof(rep));
    }
    void keyReport(uint8_t modifiers, const uint8_t *keys, uint8_t count) {
        uint8_t rep[8] = {REPORT_ID_KEYBOARD, modifiers, 0, 0, 0, 0, 0, 0};
        if (count > 6) count = 6;
        for (uint8_t i = 0; i < count; i++) rep[2 + i] = keys[i];
        sendReport(rep, sizeof(rep));
    }
};
static CMBHID hid;

// Windows VK -> HID Usage ID (常用键, 与上位机 code_to_vk 对应)
static uint8_t vkToUsage(uint16_t vk) {
    if (vk >= 'A' && vk <= 'Z') return (uint8_t)(0x04 + (vk - 'A'));
    if (vk >= '1' && vk <= '9') return (uint8_t)(0x1E + (vk - '1'));
    if (vk == '0') return 0x27;
    switch (vk) {
      case VK_RETURN: return 0x28;
      case VK_ESCAPE: return 0x29;
      case VK_BACK:   return 0x2A;
      case VK_TAB:    return 0x2B;
      case VK_SPACE:  return 0x2C;
      case VK_UP:     return 0x52;
      case VK_DOWN:   return 0x51;
      case VK_LEFT:   return 0x50;
      case VK_RIGHT:  return 0x4F;
      case VK_F1: return 0x3A; case VK_F2: return 0x3B; case VK_F3: return 0x3C;
      case VK_F4: return 0x3D; case VK_F5: return 0x3E; case VK_F6: return 0x3F;
      case VK_F7: return 0x40; case VK_F8: return 0x41; case VK_F9: return 0x42;
      case VK_F10: return 0x43; case VK_F11: return 0x44; case VK_F12: return 0x45;
      default: return 0;
    }
}

static uint8_t modsToHid(uint8_t mods) {
    uint8_t h = 0;
    if (mods & 1) h |= 0x01; // ctrl
    if (mods & 2) h |= 0x02; // shift
    if (mods & 4) h |= 0x04; // alt
    if (mods & 8) h |= 0x08; // meta
    return h;
}

static String rxLine;
static uint8_t g_key_mods = 0;
static uint8_t g_keys[6] = {0};
static uint8_t g_key_count = 0;
static uint8_t g_mouse_buttons = 0;
static bool g_have_absolute = false;
static int g_absolute_x = 0;
static int g_absolute_y = 0;
static bool g_input_active = false;
static unsigned long g_last_input_ms = 0;
static const unsigned long INPUT_TIMEOUT_MS = 2000;
static bool g_rx_overflow = false;

static void releaseAll() {
    g_key_mods = 0;
    g_key_count = 0;
    memset(g_keys, 0, sizeof(g_keys));
    g_mouse_buttons = 0;
    g_have_absolute = false;
    hid.keyReport(0, g_keys, 0);
    hid.mouseMove(0, 0, 0, 0);
    g_input_active = false;
}

static void touchInput() {
    g_last_input_ms = millis();
    g_input_active = true;
}

static int parseInts(char *text, int *values, int capacity) {
    int count = 0;
    char *p = text;
    while (p && *p && count < capacity) {
        char *comma = strchr(p, ',');
        if (comma) *comma = 0;
        char *end = nullptr;
        long value = strtol(p, &end, 10);
        if (end == p || *end != 0) return -1;
        values[count++] = (int)value;
        if (!comma) break;
        p = comma + 1;
    }
    return count;
}

void handleMouse(char *tok) {
    // M,dx,dy,l,r,s,w  或  M,x,y,1
    int v[7] = {0};
    int count = parseInts(tok, v, 7);
    if (count < 0) return;
    if (count == 3 && v[2] == 1) {
        /* The serial absolute form has exactly three numeric fields after M.
           Keep the HID descriptor relative and convert normalized coordinates
           to a bounded delta so the command cannot be mistaken for a button. */
        if (v[0] < 0) v[0] = 0;
        if (v[0] > 1000) v[0] = 1000;
        if (v[1] < 0) v[1] = 0;
        if (v[1] > 1000) v[1] = 1000;
        if (!g_have_absolute) {
            g_absolute_x = v[0];
            g_absolute_y = v[1];
            g_have_absolute = true;
            hid.mouseMove(0, 0, g_mouse_buttons, 0);
            return;
        }
        int dx = v[0] - g_absolute_x;
        int dy = v[1] - g_absolute_y;
        g_absolute_x = v[0];
        g_absolute_y = v[1];
        while (dx != 0 || dy != 0) {
            int sx = dx > 127 ? 127 : (dx < -127 ? -127 : dx);
            int sy = dy > 127 ? 127 : (dy < -127 ? -127 : dy);
            hid.mouseMove((int8_t)sx, (int8_t)sy, g_mouse_buttons, 0);
            dx -= sx;
            dy -= sy;
        }
        return;
    }
    if (count != 6) return;
    g_mouse_buttons = (v[2] ? 1 : 0) | (v[3] ? 2 : 0) | (v[4] ? 4 : 0);
    uint8_t buttons = g_mouse_buttons;
    hid.mouseMove((int8_t)v[0], (int8_t)v[1], buttons, (int8_t)v[5]);
}

void handleKey(char *tok, bool down) {
    // K,down/up,code,mods
    char *p = tok;
    char *parts[3] = {nullptr, nullptr, nullptr};
    int count = 0;
    for (int i = 0; i < 3; i++) {
        char *c = strchr(p, ',');
        parts[i] = p;
        count++;
        if (!c) break;
        *c = 0;
        p = c + 1;
    }
    if (count != 3 || !parts[1] || !parts[2]) return;
    uint16_t code = (uint16_t)atoi(parts[1]);
    uint8_t mods = (uint8_t)atoi(parts[2]);
    uint8_t usage = vkToUsage(code);
    g_key_mods = modsToHid(mods);
    if (code == VK_CONTROL || code == VK_LCONTROL || code == VK_RCONTROL) {
        if (down) g_key_mods |= (code == VK_RCONTROL ? 0x10 : 0x01);
        else g_key_mods &= (uint8_t)~(0x01 | 0x10);
    } else if (code == VK_SHIFT || code == VK_LSHIFT || code == VK_RSHIFT) {
        if (down) g_key_mods |= (code == VK_RSHIFT ? 0x20 : 0x02);
        else g_key_mods &= (uint8_t)~(0x02 | 0x20);
    } else if (code == VK_MENU || code == VK_LMENU || code == VK_RMENU) {
        if (down) g_key_mods |= (code == VK_RMENU ? 0x40 : 0x04);
        else g_key_mods &= (uint8_t)~(0x04 | 0x40);
    } else if (usage) {
        int at = -1;
        for (uint8_t i = 0; i < g_key_count; i++)
            if (g_keys[i] == usage) { at = i; break; }
        if (down && at < 0 && g_key_count < 6) g_keys[g_key_count++] = usage;
        if (!down && at >= 0) {
            g_keys[at] = g_keys[g_key_count - 1];
            g_keys[--g_key_count] = 0;
        }
    }
    hid.keyReport(g_key_mods, g_keys, g_key_count);
}

void processLine(String line) {
    if (line.length() == 0) return;
    char buf[128]; line.toCharArray(buf, sizeof(buf));
    char *p = buf;

    if (strncmp(p, "M,", 2) == 0) {
        handleMouse(p + 2);
        touchInput();
        return;
    }
    if (strncmp(p, "K,", 2) == 0) {
        bool down = strstr(p, ",down,") != NULL;
        handleKey(p + 2, down);
        touchInput();
        return;
    }
    if (strncmp(p, "P,", 2) == 0) {
        Serial.println("P,pong");
        touchInput();
        return;
    }
}

void setup() {
    Serial.begin(115200);
    hid.begin();
    // 部分平台需注册描述符: HID_.setReportDesc(...) 视框架而定
    rxLine.reserve(64);
    g_last_input_ms = millis();
}

void loop() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (!g_rx_overflow) processLine(rxLine);
            rxLine = "";
            g_rx_overflow = false;
        } else {
            if (!g_rx_overflow) {
                if (rxLine.length() < 120) rxLine += c;
                else g_rx_overflow = true;
            }
        }
    }
    if (g_input_active && millis() - g_last_input_ms > INPUT_TIMEOUT_MS)
        releaseAll();
    delay(2);
}
