/* input_bridge.c - 键鼠事件处理
 * 双通道:
 *   CH_SERIAL 协议行透传到 ESP32 (固件模拟 HID)
 *   CH_HID    本机 SendInput 直接注入 (本地回环/调试)
 * 键码: 浏览器 KeyboardEvent.code -> Windows VK; 同时保留 HID usage 给串口协议。
 */
#include "input_bridge.h"
#include "serial_link.h"
#include "udp_link.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

/* ---------- VK -> HID usage (给 UDP 通道用, 与板子固件对齐) ---------- */
static int vk_to_hid(int vk) {
    if (vk >= 'A' && vk <= 'Z') return 0x04 + (vk - 'A');
    if (vk >= '1' && vk <= '9') return 0x1E + (vk - '1');
    if (vk == '0') return 0x27;
    if (vk >= VK_F1 && vk <= VK_F12) return 0x3A + (vk - VK_F1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        if (vk == VK_NUMPAD0) return 0x62;
        return 0x58 + (vk - VK_NUMPAD0);   /* 1..9 -> 0x59..0x61 */
    }
    switch (vk) {
        case VK_RETURN:    return 0x28;
        case VK_ESCAPE:    return 0x29;
        case VK_BACK:      return 0x2A;
        case VK_TAB:       return 0x2B;
        case VK_SPACE:     return 0x2C;
        case VK_OEM_MINUS: return 0x2D;
        case VK_OEM_PLUS:  return 0x2E;
        case VK_OEM_4:     return 0x2F;
        case VK_OEM_6:     return 0x30;
        case VK_OEM_5:     return 0x31;
        case VK_OEM_1:     return 0x33;
        case VK_OEM_7:     return 0x34;
        case VK_OEM_3:     return 0x35;
        case VK_OEM_COMMA: return 0x36;
        case VK_OEM_PERIOD:return 0x37;
        case VK_OEM_2:     return 0x38;
        case VK_CAPITAL:   return 0x39;
        case VK_SNAPSHOT:  return 0x46;
        case VK_SCROLL:    return 0x47;
        case VK_PAUSE:     return 0x48;
        case VK_INSERT:    return 0x49;
        case VK_HOME:      return 0x4A;
        case VK_PRIOR:     return 0x4B;
        case VK_DELETE:    return 0x4C;
        case VK_END:       return 0x4D;
        case VK_NEXT:      return 0x4E;
        case VK_RIGHT:     return 0x4F;
        case VK_LEFT:      return 0x50;
        case VK_DOWN:      return 0x51;
        case VK_UP:        return 0x52;
        case VK_NUMLOCK:   return 0x53;
        case VK_DIVIDE:    return 0x54;
        case VK_MULTIPLY:  return 0x55;
        case VK_SUBTRACT:  return 0x56;
        case VK_ADD:       return 0x57;
        default:           return 0;
    }
}

static int vk_to_hid_mod(int vk) {
    switch (vk) {
        case VK_LCONTROL: case VK_CONTROL: return 0x01;
        case VK_LSHIFT:   case VK_SHIFT:   return 0x02;
        case VK_LMENU:    case VK_MENU:    return 0x04;
        case VK_LWIN:                      return 0x08;
        case VK_RCONTROL:                  return 0x10;
        case VK_RSHIFT:                    return 0x20;
        case VK_RMENU:                     return 0x40;
        case VK_RWIN:                      return 0x80;
        default: return 0;
    }
}

/* UDP 键盘报告状态: 修饰位 + 最多 6 个同时按下的键 */
static int g_hid_mods = 0;
static unsigned char g_hid_keys[6];
static int g_hid_n = 0;
static int g_mouse_mask = 0;

static bool hid_flush(void) {
    return udp_send_keyboard(g_hid_mods, g_hid_keys, g_hid_n);
}

static void clear_input_state(void) {
    g_state.key_count = 0;
    g_hid_n = 0;
    g_hid_mods = 0;
    g_mouse_mask = 0;
}

/* 浏览器 code -> VK (常用键全集, 覆盖字母/数字/功能/方向/符号) */
static int code_to_vk(const char *code) {
    if (!code) return 0;
    if (code[0] == 'Key' && code[1])      return VkKeyScanA(code[2]) & 0xFF;
    if (code[0] == 'D' && code[1] >= '0' && code[1] <= '9') return code[1];
    if (strncmp(code, "Digit", 5) == 0)    return code[5];
    if (strncmp(code, "Numpad", 6) == 0) {
        char c = code[6];
        if (c >= '0' && c <= '9') return VK_NUMPAD0 + (c - '0');
    }
    struct { const char *k; int vk; } map[] = {
        {"Enter", VK_RETURN}, {"Escape", VK_ESCAPE}, {"Backspace", VK_BACK},
        {"Tab", VK_TAB}, {"Space", VK_SPACE}, {"ShiftLeft", VK_LSHIFT},
        {"ShiftRight", VK_RSHIFT}, {"ControlLeft", VK_LCONTROL},
        {"ControlRight", VK_RCONTROL}, {"AltLeft", VK_LMENU},
        {"AltRight", VK_RMENU}, {"MetaLeft", VK_LWIN}, {"MetaRight", VK_RWIN},
        {"CapsLock", VK_CAPITAL}, {"ArrowUp", VK_UP}, {"ArrowDown", VK_DOWN},
        {"ArrowLeft", VK_LEFT}, {"ArrowRight", VK_RIGHT}, {"Insert", VK_INSERT},
        {"Delete", VK_DELETE}, {"Home", VK_HOME}, {"End", VK_END},
        {"PageUp", VK_PRIOR}, {"PageDown", VK_NEXT}, {"F1", VK_F1}, {"F2", VK_F2},
        {"F3", VK_F3}, {"F4", VK_F4}, {"F5", VK_F5}, {"F6", VK_F6}, {"F7", VK_F7},
        {"F8", VK_F8}, {"F9", VK_F9}, {"F10", VK_F10}, {"F11", VK_F11},
        {"F12", VK_F12}, {"Minus", VK_OEM_MINUS}, {"Equal", VK_OEM_PLUS},
        {"BracketLeft", VK_OEM_4}, {"BracketRight", VK_OEM_6},
        {"Backslash", VK_OEM_5}, {"Semicolon", VK_OEM_1}, {"Quote", VK_OEM_7},
        {"Comma", VK_OEM_COMMA}, {"Period", VK_OEM_PERIOD}, {"Slash", VK_OEM_2},
        {"Backquote", VK_OEM_3}, {"PrintScreen", VK_SNAPSHOT}, {"ScrollLock", VK_SCROLL},
        {"Pause", VK_PAUSE}, {"NumLock", VK_NUMLOCK}, {0, 0}
    };
    for (int i = 0; map[i].k; i++)
        if (strcmp(code, map[i].k) == 0) return map[i].vk;
    return 0;
}

static int mods_mask(const key_ev_t *e) {
    int m = 0;
    if (e->ctrl)  m |= 1;
    if (e->shift) m |= 2;
    if (e->alt)   m |= 4;
    if (e->meta)  m |= 8;
    return m;
}

static void inject_mouse_local(const mouse_ev_t *e) {
    INPUT in[5]; int n = 0;
    ZeroMemory(in, sizeof(in));

    if (e->absolute) {
        in[n].type = INPUT_MOUSE;
        in[n].mi.dx = MulDiv(e->x, 65535, GetSystemMetrics(SM_CXSCREEN));
        in[n].mi.dy = MulDiv(e->y, 65535, GetSystemMetrics(SM_CYSCREEN));
        in[n].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
        n++;
    } else if (e->dx || e->dy) {
        in[n].type = INPUT_MOUSE;
        in[n].mi.dx = e->dx; in[n].mi.dy = e->dy;
        in[n].mi.dwFlags = MOUSEEVENTF_MOVE;
        n++;
    }
    if (e->buttons[0]) {
        in[n].type = INPUT_MOUSE;
        in[n].mi.dwFlags = e->buttons[0] > 0 ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        n++;
    }
    if (e->buttons[1]) {
        in[n].type = INPUT_MOUSE;
        in[n].mi.dwFlags = e->buttons[1] > 0 ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        n++;
    }
    if (e->buttons[2]) {
        in[n].type = INPUT_MOUSE;
        in[n].mi.dwFlags = e->buttons[2] > 0 ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        n++;
    }
    if (e->wheel) {
        in[n].type = INPUT_MOUSE;
        in[n].mi.mouseData = e->wheel;
        in[n].mi.dwFlags = MOUSEEVENTF_WHEEL;
        n++;
    }
    if (n) SendInput(n, in, sizeof(INPUT));
}

static void inject_key_local(const key_ev_t *e) {
    INPUT in; ZeroMemory(&in, sizeof(in));
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)e->code;
    in.ki.dwFlags = e->down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(INPUT));
}

void input_dispatch_mouse(const mouse_ev_t *ev) {
    if (!g_state.grabbed) return;

    bool ok = false;
    for (int i = 0; i < 3; i++) {
        if (ev->buttons[i] > 0) g_mouse_mask |= (1 << i);
        else if (ev->buttons[i] < 0) g_mouse_mask &= ~(1 << i);
    }
    if (g_state.channel == CH_UDP) {
        int wheel = 0;
        if (ev->wheel) {
            wheel = ev->wheel / 120;
            if (wheel == 0) wheel = ev->wheel > 0 ? 1 : -1;
            if (wheel > 127) wheel = 127;
            if (wheel < -128) wheel = -128;
        }
        /* 绝对坐标在 UDP 协议里没有对应字段, 只用相对位移 */
        ok = udp_send_mouse(ev->dx, ev->dy, g_mouse_mask, wheel);
        if (ok) { g_state.events_sent++; g_state.last_event_at = now_sec(); }
        else     { g_state.events_dropped++; }
        return;
    }

    char line[CMB_MAX_LINE];
    proto_mouse(line, sizeof(line), ev->dx, ev->dy, ev->buttons, ev->wheel,
                ev->absolute, ev->x, ev->y);

    if (g_state.channel == CH_SERIAL) {
        ok = serial_send_line(line);
    } else {
        inject_mouse_local(ev);
        ok = true;
    }
    if (ok) g_state.events_sent++; else g_state.events_dropped++;
    g_state.last_event_at = now_sec();
}

void input_dispatch_key(const key_ev_t *ev) {
    if (!g_state.grabbed) return;

    /* 若给的是浏览器 code 名, 先映射成 VK */
    int vk = ev->code;
    if (ev->code_name[0] && !vk) {
        vk = code_to_vk(ev->code_name);
    }

    /* 维护按下集合 */
    bool found = false;
    for (int i = 0; i < g_state.key_count; i++)
        if (g_state.keys_pressed[i] == vk) { found = true; break; }
    if (ev->down && !found && g_state.key_count < 16)
        g_state.keys_pressed[g_state.key_count++] = vk;
    if (!ev->down) {
        for (int i = 0; i < g_state.key_count; i++)
            if (g_state.keys_pressed[i] == vk) {
                g_state.keys_pressed[i] = g_state.keys_pressed[g_state.key_count - 1];
                g_state.key_count--;
                break;
            }
    }

    char line[CMB_MAX_LINE];
    proto_key(line, sizeof(line), ev->down, vk, mods_mask(ev));

    key_ev_t local = *ev;
    local.code = vk;
    bool ok = false;

    if (g_state.channel == CH_UDP) {
        int hmod = vk_to_hid_mod(vk);
        if (hmod) {
            if (ev->down) g_hid_mods |= hmod;
            else          g_hid_mods &= ~hmod;
        } else {
            int usage = vk_to_hid(vk);
            if (usage) {
                if (ev->down) {
                    bool exists = false;
                    for (int i = 0; i < g_hid_n; i++)
                        if (g_hid_keys[i] == usage) { exists = true; break; }
                    if (!exists && g_hid_n < 6) g_hid_keys[g_hid_n++] = (unsigned char)usage;
                } else {
                    for (int i = 0; i < g_hid_n; i++)
                        if (g_hid_keys[i] == usage) {
                            g_hid_keys[i] = g_hid_keys[g_hid_n - 1];
                            g_hid_n--;
                            break;
                        }
                }
            }
        }
        /* 修饰键按本次事件的真实状态同步: 只 |= 会残留, 导致后续字符全带 Ctrl */
        g_hid_mods = 0;
        if (ev->ctrl)  g_hid_mods |= 0x01;
        if (ev->shift) g_hid_mods |= 0x02;
        if (ev->alt)   g_hid_mods |= 0x04;
        if (ev->meta)  g_hid_mods |= 0x08;
        /* 除非这个事件本身就是修饰键(它的按下/抬起由 vk_to_hid_mod 管) */
        if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) {
            if (ev->down) g_hid_mods |= 0x01; else g_hid_mods &= ~0x01;
        }
        if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) {
            if (ev->down) g_hid_mods |= 0x02; else g_hid_mods &= ~0x02;
        }
        if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) {
            if (ev->down) g_hid_mods |= 0x04; else g_hid_mods &= ~0x04;
        }
        ok = hid_flush();
    } else if (g_state.channel == CH_SERIAL) {
        ok = serial_send_line(line);
    } else {
        inject_key_local(&local);
        ok = true;
    }
    if (ok) g_state.events_sent++; else g_state.events_dropped++;
    g_state.last_event_at = now_sec();
}

void input_clear_keys(void) {
    input_release_all();
}

void input_release_all(void) {
    int pressed[16];
    int pressed_n = g_state.key_count;
    if (pressed_n > (int)(sizeof(pressed) / sizeof(pressed[0])))
        pressed_n = (int)(sizeof(pressed) / sizeof(pressed[0]));
    memcpy(pressed, g_state.keys_pressed, (size_t)pressed_n * sizeof(pressed[0]));
    int buttons = g_mouse_mask;

    /* Snapshot state before sending. A failed transport still must not leave the
       bridge believing that a key is held after focus/link loss. */
    clear_input_state();

    if (g_state.channel == CH_UDP) {
        udp_send_release_all();
        return;
    }

    if (g_state.channel == CH_SERIAL) {
        char line[CMB_MAX_LINE];
        int zero_buttons[3] = {0, 0, 0};
        proto_mouse(line, sizeof(line), 0, 0, zero_buttons, 0, false, 0, 0);
        serial_send_line(line);
        for (int i = 0; i < pressed_n; i++) {
            proto_key(line, sizeof(line), false, pressed[i], 0);
            serial_send_line(line);
        }
        return;
    }

    if (g_state.channel == CH_HID) {
        for (int i = 0; i < pressed_n; i++) {
            key_ev_t ev;
            ZeroMemory(&ev, sizeof(ev));
            ev.code = pressed[i];
            ev.down = false;
            inject_key_local(&ev);
        }
        for (int i = 0; i < 3; i++) {
            if (buttons & (1 << i)) {
                mouse_ev_t ev;
                ZeroMemory(&ev, sizeof(ev));
                ev.buttons[i] = -1;
                inject_mouse_local(&ev);
            }
        }
    }
}
