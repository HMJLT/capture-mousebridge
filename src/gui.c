/* gui.c - CaptureMouseBridge 原生 Win32 界面 (Unicode / 自绘 / 托盘)
 * 设计要点:
 *   - 全部 W 版 API + 宽字符字面量, 杜绝中文乱码
 *   - 自绘卡片/按钮/下拉/开关, 只在按键按下时短暂 SetCapture, 不影响控件点击
 *   - 视频子窗口独立重绘, 仅在收到新帧时刷新, 主窗口 400ms 轻量重绘
 */
#include "gui.h"
#include "capture.h"
#include "link.h"
#include "serial_link.h"
#include "input_bridge.h"
#include "udp_link.h"
#include "lan_mcp.h"
#include <windows.h>
#include <windowsx.h>
#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "shell32.lib")

/* ---------- 配色 ---------- */
#define C_BG      RGB(14, 17, 22)
#define C_CARD    RGB(22, 26, 33)
#define C_INSET   RGB(27, 32, 40)
#define C_BORDER  RGB(38, 44, 55)
#define C_TEXT    RGB(232, 236, 242)
#define C_MUTED   RGB(152, 162, 179)
#define C_DIM     RGB(107, 118, 136)
#define C_ACCENT  RGB(76, 141, 255)
#define C_ACC_DK  RGB(38, 74, 138)
#define C_OK      RGB(52, 196, 107)
#define C_WARN    RGB(224, 160, 60)
#define C_ERR     RGB(234, 90, 82)
#define C_ACC_TXT RGB(150, 190, 255)
#define C_WHITE   RGB(255, 255, 255)

/* ---------- 控件 ID ---------- */
enum {
    ID_NONE = 0,
    ID_BTN_TRAY, ID_BTN_COPY,
    ID_BTN_REFRESH,
    ID_SEG_SERIAL, ID_SEG_UDP, ID_SEG_HID,
    ID_COMBO_VIDEO, ID_COMBO_SERIAL, ID_COMBO_BAUD,
    ID_BTN_CONNECT, ID_BTN_DISCONNECT, ID_BTN_DISCOVER,
    ID_SW_GRAB, ID_SW_LAN,
    ID_POP_AREA = 1900,      /* 下拉列表空白处 */
    ID_POP_BASE = 2000,      /* 列表项: ID_POP_BASE + 索引 */
};

/* 消息 */
#define WM_APP_POPUP_PICK (WM_APP + 100)
#define WM_TRAY           (WM_APP + 101)
#define TRAY_ID           1
#define TIMER_UI           1
#define TIMER_RESUME       2

/* ---------- 全局 ---------- */
static HINSTANCE g_inst;
static HWND  g_hwnd = NULL, g_video = NULL;
static HFONT g_fTitle, g_fHead, g_fBody, g_fSmall, g_fNum;
static int   g_dpi = 96;

static bool  g_tray_added = false;
static HICON g_icon = NULL;
static bool  g_in_tray = false;
static int   g_fps_backup = 0;   /* 进托盘前的预览帧率, 恢复时还原 */
static bool  g_cap_was_active = false;  /* 进托盘前是否正在预览 */
static int   g_resume_tries = 0;
static bool  g_btn_hover = 0;
static int   g_pressed = 0;

static video_dev_t  g_videos[16];
static serial_port_t g_ports[CMB_MAX_PORTS];
static int g_video_count = 0, g_port_count = 0;
static const int g_bauds[] = {9600, 19200, 38400, 57600, 115200, 230400};
static int g_baud_sel = 4;

/* 键鼠接管鼠标跟踪 */
static bool g_grabbed_cursor_hidden = false;

/* ---------- 布局 ---------- */
typedef struct {
    RECT client, header, videoCard, guideCard;
    RECT cardSrc, cardLink, cardGrab, cardMcp, cardLog;
    RECT comboVideo, btnRefresh;
    RECT segSerial, segUdp, segHid;
    RECT comboSerial, comboBaud;
    RECT editHost, editUdpPort;
    RECT btnConnect, btnDisconnect, btnDiscover;
    RECT swGrab;
    RECT swLan, editPort;
    RECT btnTray, btnCopy, pillStatus;
    RECT logInner;
} ui_t;

static ui_t g_ui;

static int S(int px) { return MulDiv(px, g_dpi, 96); }

/* UTF-8 优先, 失败回退系统 ANSI(GBK), 覆盖设备名两种来源 */
static void narrow_to_wide(const char *s, wchar_t *out, int cap) {
    out[0] = 0;
    if (!s || !s[0]) return;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap) > 0) return;
    out[0] = 0;
    MultiByteToWideChar(CP_ACP, 0, s, -1, out, cap);
}

static RECT R(int l, int t, int r, int b) { RECT x = {l, t, r, b}; return x; }
static RECT inset(RECT r, int d) { return R(r.left + d, r.top + d, r.right - d, r.bottom - d); }
static int  rw(RECT r) { return r.right - r.left; }
static int  rh(RECT r) { return r.bottom - r.top; }

/* ---------- 绘制基础 ---------- */
static void fill_rr(HDC dc, RECT r, int rad, COLORREF c) {
    HBRUSH br = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad * 2, rad * 2);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(br);
}

static void frame_rr(HDC dc, RECT r, int rad, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ op = SelectObject(dc, p);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad * 2, rad * 2);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(p);
}

static void fill_rect(HDC dc, RECT r, COLORREF c) {
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &r, br);
    DeleteObject(br);
}

static void txt(HDC dc, RECT r, const wchar_t *s, HFONT f, COLORREF c, UINT fmt) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    SelectObject(dc, f);
    DrawTextW(dc, s, -1, &r, fmt);
}

static void dot(HDC dc, int cx, int cy, int r, COLORREF c) {
    HBRUSH br = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(br);
}

static void chevron(HDC dc, int cx, int cy, COLORREF c) {
    POINT p[3] = {{cx - S(4), cy - S(2)}, {cx + S(4), cy - S(2)}, {cx, cy + S(3)}};
    HBRUSH br = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, p, 3);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(br);
}

/* 按钮: kind 0 次级 / 1 主按钮 */
static void draw_btn(HDC dc, RECT r, const wchar_t *label, int kind, bool hover, bool disabled) {
    int rad = S(7);
    if (disabled) {
        fill_rr(dc, r, rad, C_INSET);
        frame_rr(dc, r, rad, C_BORDER);
        txt(dc, r, label, g_fBody, C_DIM, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    if (kind == 1) {
        fill_rr(dc, r, rad, hover ? RGB(96, 156, 255) : C_ACCENT);
        txt(dc, r, label, g_fBody, C_WHITE, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        fill_rr(dc, r, rad, hover ? RGB(36, 43, 54) : C_INSET);
        frame_rr(dc, r, rad, hover ? RGB(58, 68, 84) : C_BORDER);
        txt(dc, r, label, g_fBody, C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

static void draw_combo(HDC dc, RECT r, const wchar_t *label, bool enabled) {
    fill_rr(dc, r, S(7), C_INSET);
    frame_rr(dc, r, S(7), C_BORDER);
    RECT tr = r;
    tr.left += S(10);
    tr.right -= S(22);
    txt(dc, tr, label, g_fBody, enabled ? C_TEXT : C_DIM,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    chevron(dc, r.right - S(13), (r.top + r.bottom) / 2, enabled ? C_MUTED : C_DIM);
}

/* 多行文本块垂直居中绘制 (DT_VCENTER 只在单行生效, 这里先量再偏移) */
static void txt_block_center(HDC dc, RECT rc, const wchar_t *s, HFONT f,
                             COLORREF c, UINT extra_fmt) {
    RECT calc = rc;
    SelectObject(dc, f);
    DrawTextW(dc, s, -1, &calc, DT_CALCRECT | DT_WORDBREAK | DT_CENTER);
    int h = calc.bottom - calc.top;
    int y = rc.top + (rh(rc) - h) / 2;
    RECT tr = R(rc.left, y, rc.right, y + h);
    txt(dc, tr, s, f, c, DT_CENTER | DT_WORDBREAK | extra_fmt);
}

static void draw_switch(HDC dc, RECT r, bool on) {
    int h = rh(r);
    int w = S(40);
    RECT track = R(r.left, r.top + (h - S(22)) / 2, r.left + w, r.top + (h + S(22)) / 2);
    fill_rr(dc, track, S(11), on ? C_ACCENT : RGB(50, 57, 70));
    int kr = S(8);
    int cx = on ? track.right - S(11) : track.left + S(11);
    int cy = (track.top + track.bottom) / 2;
    dot(dc, cx, cy, kr, on ? C_WHITE : RGB(160, 170, 186));
}

/* ---------- 设备枚举 ---------- */
static void reload_devices(void) {
    g_video_count = capture_enum(g_videos, 16);
    g_port_count = serial_enumerate(g_ports, CMB_MAX_PORTS);
}

static const wchar_t *video_label(wchar_t *buf, int cap) {
    if (g_video_count <= 0) { wcscpy_s(buf, cap, L"未检测到采集卡"); return buf; }
    if (g_state.video_source >= 0) {
        for (int i = 0; i < g_video_count; i++)
            if (g_videos[i].index == g_state.video_source) {
                narrow_to_wide(g_videos[i].name, buf, cap);
                return buf;
            }
    }
    wcscpy_s(buf, cap, L"请选择采集源");
    return buf;
}

static const wchar_t *serial_label(wchar_t *buf, int cap) {
    if (g_port_count <= 0) { wcscpy_s(buf, cap, L"未检测到串口"); return buf; }
    const char *port = g_state.serial_port[0] ? g_state.serial_port : g_ports[0].port;
    for (int i = 0; i < g_port_count; i++) {
        if (_stricmp(g_ports[i].port, port) == 0) {
            wchar_t desc[256];
            narrow_to_wide(g_ports[i].description, desc, 256);
            swprintf_s(buf, cap, L"%hs · %s", g_ports[i].port, desc);
            return buf;
        }
    }
    MultiByteToWideChar(CP_ACP, 0, port, -1, buf, cap);
    return buf;
}

static void baud_label(wchar_t *buf, int cap) {
    swprintf_s(buf, cap, L"%d", g_state.serial_baud);
}

/* ---------- 状态派生 ---------- */
static const wchar_t *link_text(void) {
    switch (g_state.link_state) {
        case LINK_CONNECTED:    return L"已连接";
        case LINK_CONNECTING:   return L"连接中";
        case LINK_DISCONNECTED: return L"已断开";
        case LINK_FAILED:       return L"连接失败";
        default:                return L"未连接";
    }
}
static COLORREF link_color(void) {
    if (g_state.link_state == LINK_CONNECTED) return C_OK;
    if (g_state.link_state == LINK_CONNECTING) return C_WARN;
    if (g_state.link_state == LINK_FAILED || g_state.link_state == LINK_DISCONNECTED) return C_ERR;
    return C_DIM;
}

typedef struct { const wchar_t *short_name; const wchar_t *title; bool ok; } step_t;

static void build_steps(step_t steps[5]) {
    steps[0].short_name = L"采集卡"; steps[0].title = L"检查采集卡是否出画面"; steps[0].ok = g_state.capturing;
    steps[1].short_name = L"ESP32 上电"; steps[1].title = L"确认 ESP32 上电并被识别"; steps[1].ok = g_state.link_device[0] != 0;
    if (g_state.channel == CH_UDP) {
        steps[2].short_name = L"UDP 地址";
        steps[2].title = L"确认板子 IP 并连上 UDP 4210";
    } else if (g_state.channel == CH_SERIAL) {
        steps[2].short_name = L"串口波特率";
        steps[2].title = L"选择串口与波特率并连接";
    } else {
        steps[2].short_name = L"本机注入";
        steps[2].title = L"本机注入无需链路";
    }
    steps[2].ok = g_state.link_state == LINK_CONNECTED;
    steps[3].short_name = L"心跳"; steps[3].title = L"链路心跳正常"; steps[3].ok = g_state.last_ok_at > 0;
    steps[4].short_name = L"键鼠接管"; steps[4].title = L"开启键鼠接管并验证"; steps[4].ok = g_state.grabbed;
}

/* ---------- 绘制: 各卡片 ---------- */
static void paint_header(HDC dc) {
    RECT r = g_ui.header;
    fill_rect(dc, r, C_BG);

    /* 图标 */
    RECT ic = R(r.left, r.top + S(8), r.left + S(34), r.top + S(42));
    fill_rr(dc, ic, S(9), C_ACCENT);
    POINT tp[3] = {{ic.left + S(12), ic.top + S(9)}, {ic.left + S(12), ic.top + S(25)}, {ic.left + S(26), ic.top + S(17)}};
    HBRUSH wb = CreateSolidBrush(C_WHITE);
    HGDIOBJ ob = SelectObject(dc, wb);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, tp, 3);
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(wb);

    txt(dc, R(r.left + S(46), r.top + S(6), r.left + S(260), r.top + S(28)),
        L"采集卡桥", g_fTitle, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    txt(dc, R(r.left + S(46), r.top + S(26), r.left + S(300), r.top + S(46)),
        L"采集画面 · 键鼠透传 · ESP32 重连", g_fSmall, C_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    /* 状态胶囊 */
    RECT pill = g_ui.pillStatus;
    fill_rr(dc, pill, S(11), C_INSET);
    frame_rr(dc, pill, S(11), C_BORDER);
    dot(dc, pill.left + S(13), (pill.top + pill.bottom) / 2, S(4), link_color());
    txt(dc, R(pill.left + S(24), pill.top, pill.right - S(25), pill.bottom),
        link_text(), g_fSmall, C_MUTED, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    draw_btn(dc, g_ui.btnTray, L"最小化到托盘", 0, g_btn_hover == ID_BTN_TRAY, false);
}

static void paint_video_card(HDC dc) {
    RECT card = g_ui.videoCard;
    fill_rr(dc, card, S(10), C_CARD);
    frame_rr(dc, card, S(10), C_BORDER);

    /* 顶部细信息行 */
    wchar_t left[128];
    if (g_state.capturing)
        swprintf_s(left, 128, L"实时预览  %dx%d  ·  %.0f fps",
                   g_state.cap_width, g_state.cap_height, g_state.cap_fps);
    else
        wcscpy_s(left, 128, L"实时预览");

    txt(dc, R(card.left + S(14), card.top + S(8), card.right - S(120), card.top + S(28)),
        left, g_fSmall, C_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    wchar_t right[96];
    if (g_state.capturing && g_state.cap_latency_ms >= 0)
        swprintf_s(right, 96, L"缓冲 %d ms · 已接收 %ld 帧", g_state.cap_latency_ms, g_state.cap_frames);
    else
        swprintf_s(right, 96, L"已接收 %ld 帧", g_state.cap_frames);
    txt(dc, R(card.right - S(220), card.top + S(8), card.right - S(14), card.top + S(28)),
        right, g_fSmall, g_state.cap_latency_ms > 90 ? C_WARN : C_DIM,
        DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    /* 视频子窗口位置在 layout 里 MoveWindow; 这里补一层边说明 */
}

static void paint_guide(HDC dc) {
    RECT card = g_ui.guideCard;
    fill_rr(dc, card, S(10), C_CARD);
    frame_rr(dc, card, S(10), C_BORDER);

    step_t steps[5];
    build_steps(steps);
    int blocked = 0;
    for (int i = 0; i < 5; i++) if (!steps[i].ok) { blocked = i + 1; break; }

    txt(dc, R(card.left + S(14), card.top + S(8), card.left + S(150), card.top + S(30)),
        L"ESP32 断线重连向导", g_fHead, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    wchar_t hint[160];
    if (blocked == 0) wcscpy_s(hint, 160, L"全部就绪，可以开始操作被控端");
    else swprintf_s(hint, 160, L"当前卡点：第 %d 步 · %s", blocked, steps[blocked - 1].title);
    txt(dc, R(card.right - S(360), card.top + S(8), card.right - S(14), card.top + S(30)),
        hint, g_fSmall, blocked == 0 ? C_OK : C_WARN, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    /* 5 个步骤胶囊, 均分宽度 */
    int pad = S(14);
    int gap = S(8);
    int avail = rw(card) - pad * 2 - gap * 4;
    int pw = avail / 5;
    int y0 = card.top + S(40), hh = S(32);
    for (int i = 0; i < 5; i++) {
        RECT pr = R(card.left + pad + i * (pw + gap), y0, card.left + pad + i * (pw + gap) + pw, y0 + hh);
        fill_rr(dc, pr, S(7), steps[i].ok ? RGB(22, 44, 32) : C_INSET);
        frame_rr(dc, pr, S(7), steps[i].ok ? RGB(40, 92, 60) : C_BORDER);
        dot(dc, pr.left + S(13), (pr.top + pr.bottom) / 2, S(3),
            steps[i].ok ? C_OK : C_DIM);
        wchar_t line[64];
        swprintf_s(line, 64, L"%d  %s", i + 1, steps[i].short_name);
        txt(dc, R(pr.left + S(22), pr.top, pr.right - S(8), pr.bottom),
            line, g_fSmall, steps[i].ok ? C_TEXT : C_MUTED,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

static void paint_src_card(HDC dc) {
    RECT card = g_ui.cardSrc;
    fill_rr(dc, card, S(10), C_CARD);
    frame_rr(dc, card, S(10), C_BORDER);
    txt(dc, R(card.left + S(14), card.top + S(10), card.left + S(120), card.top + S(30)),
        L"视频源", g_fHead, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    wchar_t tmp[128];
    draw_combo(dc, g_ui.comboVideo, video_label(tmp, 128), g_video_count > 0);
    draw_btn(dc, g_ui.btnRefresh, L"刷新", 0, g_btn_hover == ID_BTN_REFRESH, false);
}

static void paint_link_card(HDC dc) {
    RECT card = g_ui.cardLink;
    fill_rr(dc, card, S(10), C_CARD);
    frame_rr(dc, card, S(10), C_BORDER);
    txt(dc, R(card.left + S(14), card.top + S(10), card.left + S(120), card.top + S(30)),
        L"链路", g_fHead, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    /* 分段: 串口 / UDP / 本机 */
    bool serial_on = g_state.channel == CH_SERIAL;
    bool udp_on    = g_state.channel == CH_UDP;
    bool hid_on    = g_state.channel == CH_HID;
    RECT a = g_ui.segSerial, b = g_ui.segUdp, c = g_ui.segHid;
    fill_rr(dc, a, S(6), serial_on ? C_ACC_DK : C_INSET);
    if (serial_on) frame_rr(dc, a, S(6), C_ACCENT);
    fill_rr(dc, b, S(6), udp_on ? C_ACC_DK : C_INSET);
    if (udp_on) frame_rr(dc, b, S(6), C_ACCENT);
    fill_rr(dc, c, S(6), hid_on ? C_ACC_DK : C_INSET);
    if (hid_on) frame_rr(dc, c, S(6), C_ACCENT);
    txt(dc, a, L"串口", g_fSmall, serial_on ? C_ACC_TXT : C_MUTED,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    txt(dc, b, L"UDP", g_fSmall, udp_on ? C_ACC_TXT : C_MUTED,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    txt(dc, c, L"本机", g_fSmall, hid_on ? C_ACC_TXT : C_MUTED,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    wchar_t tmp[160];
    if (udp_on) {
        /* 板子地址显示在标题行右侧, 输入框本身由子窗口绘制 */
        wchar_t uh[80];
        if (g_state.udp_discovering)
            wcscpy_s(uh, 80, L"查找板子…");
        else if (g_state.udp_host[0])
            swprintf_s(uh, 80, L"%hs:%d", g_state.udp_host, g_state.udp_port);
        else
            wcscpy_s(uh, 80, L"未发现板子");
        txt(dc, R(card.left + S(60), card.top + S(10), card.right - S(14), card.top + S(30)),
            uh, g_fNum, g_state.link_state == LINK_CONNECTED ? C_OK : C_MUTED,
            DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        /* 输入框边框（内容由子窗口绘制） */
        frame_rr(dc, g_ui.editHost, S(7), C_BORDER);
        frame_rr(dc, g_ui.editUdpPort, S(7), C_BORDER);
    } else if (serial_on) {
        draw_combo(dc, g_ui.comboSerial, serial_label(tmp, 160),
                   g_port_count > 0 && serial_on);
        wchar_t bl[16];
        baud_label(bl, 16);
        draw_combo(dc, g_ui.comboBaud, bl, serial_on);
    } else {
        txt(dc, R(card.left + S(14), g_ui.comboSerial.top + S(4),
                  card.right - S(14), g_ui.comboSerial.top + S(26)),
            L"不需要链路：事件直接注入本机", g_fSmall, C_MUTED,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        txt(dc, R(card.left + S(14), g_ui.comboBaud.top + S(4),
                  card.right - S(14), g_ui.comboBaud.top + S(26)),
            L"用于本地回环测试", g_fSmall, C_MUTED,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    bool connected = g_state.link_state == LINK_CONNECTED;
    draw_btn(dc, g_ui.btnDiscover,
             g_state.udp_discovering ? L"查找中…" : L"自动发现", 0,
             g_btn_hover == ID_BTN_DISCOVER, g_state.udp_discovering);
    draw_btn(dc, g_ui.btnConnect, L"连接", 1, g_btn_hover == ID_BTN_CONNECT, connected);
    draw_btn(dc, g_ui.btnDisconnect, L"断开", 0, g_btn_hover == ID_BTN_DISCONNECT,
             g_state.link_state == LINK_IDLE);
}

static void paint_grab_card(HDC dc) {
    RECT card = g_ui.cardGrab;
    fill_rr(dc, card, S(10), C_CARD);
    frame_rr(dc, card, S(10), C_BORDER);

    draw_switch(dc, g_ui.swGrab, g_state.grabbed);
    txt(dc, R(g_ui.swGrab.right + S(10), card.top + S(10), card.right - S(14), card.top + S(30)),
        L"接管被控端鼠标键盘", g_fBody, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    const wchar_t *sub;
    COLORREF sub_c;
    if (g_state.grabbed && g_state.link_state != LINK_CONNECTED) {
        sub = L"链路未连接：按键会被丢弃，请先点「连接」";
        sub_c = C_WARN;
    } else if (g_state.grabbed) {
        sub = L"已接管：在左侧画面区操作，按 Esc 释放";
        sub_c = C_OK;
    } else {
        sub = L"开启后，画面区的鼠标键盘将转发到被控端";
        sub_c = C_DIM;
    }
    txt(dc, R(card.left + S(14), card.top + S(32), card.right - S(14), card.top + S(52)),
        sub, g_fSmall, sub_c,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void paint_mcp_card(HDC dc) {
    RECT card = g_ui.cardMcp;
    fill_rr(dc, card, S(10), C_CARD);
    frame_rr(dc, card, S(10), C_BORDER);
    txt(dc, R(card.left + S(14), card.top + S(10), card.left + S(150), card.top + S(30)),
        L"MCP 接口", g_fHead, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    /* 本机管道 */
    dot(dc, card.left + S(18), card.top + S(44), S(4), C_OK);
    txt(dc, R(card.left + S(28), card.top + S(34), card.right - S(60), card.top + S(54)),
        L"本机管道已就绪", g_fSmall, C_MUTED, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_btn(dc, g_ui.btnCopy, L"复制", 0, g_btn_hover == ID_BTN_COPY, false);

    txt(dc, R(card.left + S(14), card.top + S(56), card.right - S(14), card.top + S(74)),
        L"\\\\.\\pipe\\cmb-mcp", g_fSmall, C_DIM,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    /* 分隔线 */
    RECT ln = R(card.left + S(14), card.top + S(80), card.right - S(14), card.top + S(81));
    fill_rect(dc, ln, C_BORDER);

    /* 局域网开关 + 端口 */
    draw_switch(dc, g_ui.swLan, g_state.lan_enabled);
    txt(dc, R(g_ui.swLan.right + S(10), card.top + S(86), g_ui.editPort.left - S(46), card.top + S(110)),
        L"局域网访问", g_fSmall, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    txt(dc, R(g_ui.editPort.left - S(44), card.top + S(86), g_ui.editPort.left - S(6), card.top + S(110)),
        L"端口", g_fSmall, C_DIM, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    /* 状态行 */
    wchar_t st[160];
    COLORREF sc = C_DIM;
    if (g_state.lan_state == 1) {
        swprintf_s(st, 160, L"监听 %hs · %d 个客户端", g_state.lan_addr, g_state.lan_clients);
        sc = C_OK;
    } else if (g_state.lan_state == 2) {
        narrow_to_wide(g_state.lan_error, st, 160);
        sc = C_ERR;
    } else {
        wcscpy_s(st, 160, L"关闭时仅本机可用，开启后局域网 Agent 可连");
    }
    txt(dc, R(card.left + S(14), card.top + S(118), card.right - S(14), card.top + S(138)),
        st, g_fSmall, sc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void paint_log_card(HDC dc, bool repaint_text) {
    RECT card = g_ui.cardLog;
    fill_rr(dc, card, S(10), C_CARD);
    frame_rr(dc, card, S(10), C_BORDER);
    txt(dc, R(card.left + S(14), card.top + S(10), card.left + S(120), card.top + S(30)),
        L"事件日志", g_fHead, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT inner = g_ui.logInner;
    static char buf[4096];
    static wchar_t wbuf[4096];
    static int last_seq = -1;
    if (repaint_text || last_seq != g_log_seq) {
        cmb_log_tail_text(buf, sizeof(buf), 40);
        narrow_to_wide(buf, wbuf, 4096);
        last_seq = g_log_seq;
    }

    /* 从底部往上画, 最多填满 */
    int line_h = S(17);
    int max_lines = rh(inner) / line_h;
    if (max_lines < 1) return;

    /* 拆分行 */
    int total = 0;
    wchar_t *lines[40];
    wchar_t *p = wbuf;
    while (*p && total < 40) {
        lines[total++] = p;
        while (*p && *p != L'\n') p++;
        if (*p == L'\n') { *p = 0; p++; }
    }
    int first = total - max_lines;
    if (first < 0) first = 0;

    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_fSmall);
    int y = inner.top;
    for (int i = first; i < total; i++) {
        COLORREF c = C_DIM;
        wchar_t *s = lines[i];
        if (wcsstr(s, L"error")) c = C_ERR;
        else if (wcsstr(s, L"warn")) c = C_WARN;
        else if (wcsstr(s, L"ok")) c = C_MUTED;
        SetTextColor(dc, c);
        RECT lr = R(inner.left, y, inner.right, y + line_h);
        DrawTextW(dc, s, -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += line_h;
    }
}

/* ---------- 主窗口绘制 ---------- */
static void paint_all(HDC dc) {
    RECT rc = g_ui.client;
    fill_rect(dc, rc, C_BG);

    paint_header(dc);
    paint_video_card(dc);
    paint_guide(dc);
    paint_src_card(dc);
    paint_link_card(dc);
    paint_grab_card(dc);
    paint_mcp_card(dc);
    paint_log_card(dc, false);
}

/* ---------- 布局 ---------- */
static void layout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    g_ui.client = rc;
    int W = rc.right, H = rc.bottom;
    int pad = S(16), gap = S(12), headerH = S(48), sideW = S(330);

    g_ui.header = R(pad, pad, W - pad, pad + headerH);
    g_ui.pillStatus = R(W - pad - S(300), pad + S(10), W - pad - S(190), pad + S(38));
    g_ui.btnTray = R(W - pad - S(180), pad + S(10), W - pad, pad + S(38));

    int y0 = pad + headerH + gap;
    int bottom = H - pad;
    int sideX = W - pad - sideW;
    int leftX = pad;
    int leftW = sideX - gap - leftX;

    int guideH = S(88);
    g_ui.videoCard = R(leftX, y0, leftX + leftW, bottom - guideH - gap);
    g_ui.guideCard = R(leftX, bottom - guideH, leftX + leftW, bottom);

    int c1 = S(84), c2 = S(178), c3 = S(74), c4 = S(150);
    int y = y0;
    g_ui.cardSrc  = R(sideX, y, sideX + sideW, y + c1); y += c1 + gap;
    g_ui.cardLink = R(sideX, y, sideX + sideW, y + c2); y += c2 + gap;
    g_ui.cardGrab = R(sideX, y, sideX + sideW, y + c3); y += c3 + gap;
    g_ui.cardMcp  = R(sideX, y, sideX + sideW, y + c4); y += c4 + gap;
    int logH = bottom - y;
    if (logH < S(56)) logH = S(56);
    g_ui.cardLog = R(sideX, y, sideX + sideW, y + logH);

    /* 视频源卡: 标题行 + combo + 刷新 */
    RECT c = g_ui.cardSrc;
    int ix = c.left + S(14), ir = c.right - S(14);
    g_ui.comboVideo = R(ix, c.top + S(34), ir - S(64), c.top + S(34) + S(30));
    g_ui.btnRefresh = R(ir - S(56), c.top + S(34), ir, c.top + S(34) + S(30));

    /* 链路卡 */
    c = g_ui.cardLink;
    int segY = c.top + S(36);
    int segW = (ir - ix - S(12)) / 3;
    g_ui.segSerial = R(ix, segY, ix + segW, segY + S(28));
    g_ui.segUdp    = R(ix + segW + S(6), segY, ix + segW * 2 + S(6), segY + S(28));
    g_ui.segHid    = R(ix + segW * 2 + S(12), segY, ir, segY + S(28));
    int half = (ir - ix - S(6)) / 2;
    g_ui.comboSerial = R(ix, segY + S(36), ir, segY + S(36) + S(28));
    g_ui.comboBaud = R(ix, segY + S(72), ir, segY + S(72) + S(28));
    g_ui.editHost = R(ix, segY + S(36), ir, segY + S(36) + S(28));
    g_ui.editUdpPort = R(ix, segY + S(72), ir, segY + S(72) + S(28));
    int bw = (ir - ix - S(10)) / 3;
    int by = segY + S(108);
    g_ui.btnDiscover   = R(ix, by, ix + bw, by + S(30));
    g_ui.btnConnect    = R(ix + bw + S(5), by, ix + bw * 2 + S(5), by + S(30));
    g_ui.btnDisconnect = R(ix + bw * 2 + S(10), by, ir, by + S(30));

    /* 接管卡 */
    c = g_ui.cardGrab;
    g_ui.swGrab = R(c.left + S(14), c.top + S(8), c.left + S(54), c.top + S(32));

    /* MCP 卡 */
    c = g_ui.cardMcp;
    ix = c.left + S(14); ir = c.right - S(14);
    g_ui.btnCopy = R(ir - S(50), c.top + S(34), ir, c.top + S(56));
    g_ui.swLan = R(ix, c.top + S(86), ix + S(40), c.top + S(110));
    g_ui.editPort = R(ir - S(56), c.top + S(88), ir, c.top + S(110));

    /* 日志内区 */
    c = g_ui.cardLog;
    g_ui.logInner = R(c.left + S(14), c.top + S(32), c.right - S(14), c.bottom - S(10));

    /* 视频子窗口 */
    RECT vi = inset(g_ui.videoCard, S(1));
    vi.top = g_ui.videoCard.top + S(30);
    vi.left = g_ui.videoCard.left + S(10);
    vi.right = g_ui.videoCard.right - S(10);
    vi.bottom = g_ui.videoCard.bottom - S(10);
    MoveWindow(g_video, vi.left, vi.top, rw(vi), rh(vi), TRUE);

    /* 端口输入框 / UDP 的 IP 与端口输入框 */
    if (g_ui.editPort.left > 0) {
        RECT ep = g_ui.editPort;
        MoveWindow(GetDlgItem(hwnd, 900), ep.left, ep.top, rw(ep), rh(ep), TRUE);
    }
    bool udp_on = g_state.channel == CH_UDP;
    HWND eh = GetDlgItem(hwnd, 901), ep2 = GetDlgItem(hwnd, 902);
    if (eh) {
        ShowWindow(eh, udp_on ? SW_SHOW : SW_HIDE);
        if (udp_on) {
            RECT r2 = inset(g_ui.editHost, S(1));
            MoveWindow(eh, r2.left, r2.top, rw(r2), rh(r2), TRUE);
        }
    }
    if (ep2) {
        ShowWindow(ep2, udp_on ? SW_SHOW : SW_HIDE);
        if (udp_on) {
            RECT r2 = inset(g_ui.editUdpPort, S(1));
            MoveWindow(ep2, r2.left, r2.top, rw(r2), rh(r2), TRUE);
        }
    }
}

/* ---------- 下拉列表 (在主窗口内绘制, 不弹独立窗口) ---------- */
static bool  g_popup_open = false;
static int   g_popup_owner = 0;
static int   g_popup_sel = 0;
static int   g_popup_hover = -1;
static RECT  g_popup_rect;
static wchar_t g_popup_items[16][128];
static int   g_popup_count = 0;

static void popup_close(void) {
    if (!g_popup_open) return;
    g_popup_open = false;
    g_popup_hover = -1;
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

static void popup_open(HWND owner, int id) {
    popup_close();
    g_popup_owner = id;
    g_popup_count = 0;
    g_popup_sel = -1;

    if (id == ID_COMBO_VIDEO) {
        for (int i = 0; i < g_video_count; i++) {
            narrow_to_wide(g_videos[i].name, g_popup_items[i], 128);
            if (g_videos[i].index == g_state.video_source) g_popup_sel = i;
            g_popup_count++;
        }
    } else if (id == ID_COMBO_SERIAL) {
        for (int i = 0; i < g_port_count; i++) {
            wchar_t desc[256];
            narrow_to_wide(g_ports[i].description, desc, 256);
            swprintf_s(g_popup_items[i], 128, L"%hs · %s", g_ports[i].port, desc);
            if (_stricmp(g_ports[i].port, g_state.serial_port) == 0) g_popup_sel = i;
            g_popup_count++;
        }
    } else if (id == ID_COMBO_BAUD) {
        for (int i = 0; i < 6; i++) {
            swprintf_s(g_popup_items[i], 128, L"%d", g_bauds[i]);
            if (g_bauds[i] == g_state.serial_baud) g_popup_sel = i;
            g_popup_count++;
        }
    }
    if (g_popup_count == 0) {
        cmb_log("warn", "下拉列表为空 id=%d", id);
        return;
    }

    RECT anchor;
    RECT cr;
    GetClientRect(g_hwnd, &cr);
    if (id == ID_COMBO_VIDEO) anchor = g_ui.comboVideo;
    else if (id == ID_COMBO_SERIAL) anchor = g_ui.comboSerial;
    else anchor = g_ui.comboBaud;

    int itemH = S(28);
    int h = g_popup_count * itemH + S(8);
    int top = anchor.bottom + S(2);
    if (top + h > cr.bottom - S(16)) top = anchor.bottom - h - S(4);
    if (top < S(8)) top = S(8);
    g_popup_rect = R(anchor.left, top, anchor.left + rw(anchor), top + h);
    g_popup_open = true;
    g_popup_hover = -1;
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void popup_commit(int idx) {
    if (idx < 0 || idx >= g_popup_count) { popup_close(); return; }
    int owner = g_popup_owner;
    popup_close();

    if (owner == ID_COMBO_VIDEO) {
        int dev = g_videos[idx].index;
        if (g_state.video_source != dev || !g_state.capturing) {
            g_state.video_source = dev;
            capture_start(dev, 1280, 720, g_state.fps_cap);
        }
        layout(g_hwnd);
        InvalidateRect(g_hwnd, NULL, FALSE);
    } else if (owner == ID_COMBO_SERIAL) {
        strncpy_s(g_state.serial_port, sizeof(g_state.serial_port), g_ports[idx].port, _TRUNCATE);
        layout(g_hwnd);
        InvalidateRect(g_hwnd, NULL, FALSE);
    } else if (owner == ID_COMBO_BAUD) {
        g_state.serial_baud = g_bauds[idx];
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}

static void popup_paint(HDC dc) {
    RECT rc = g_popup_rect;
    /* 外圈压暗一圈, 让列表看起来浮在卡片之上 */
    RECT sh = rc;
    sh.left -= S(1); sh.top -= S(1); sh.right += S(1); sh.bottom += S(1);
    frame_rr(dc, sh, S(7), RGB(6, 8, 11));
    fill_rr(dc, rc, S(6), RGB(23, 28, 36));
    frame_rr(dc, rc, S(6), RGB(52, 61, 76));

    int itemH = S(28);
    SelectObject(dc, g_fBody);
    SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < g_popup_count; i++) {
        RECT ir = R(rc.left + S(4), rc.top + S(4) + i * itemH,
                    rc.right - S(4), rc.top + S(4) + (i + 1) * itemH);
        if (i == g_popup_hover) fill_rr(dc, ir, S(4), RGB(36, 43, 54));
        else if (i == g_popup_sel) fill_rr(dc, ir, S(4), C_ACC_DK);
        SetTextColor(dc, i == g_popup_sel ? C_ACC_TXT : C_TEXT);
        RECT tr = ir;
        tr.left += S(8);
        tr.right -= S(6);
        DrawTextW(dc, g_popup_items[i], -1, &tr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

/* ---------- 视频子窗口 ---------- */
static void video_hint(HDC dc, RECT rc) {
    wchar_t msg[220];
    if (g_state.cap_error[0]) {
        narrow_to_wide(g_state.cap_error, msg, 220);
    } else if (g_video_count <= 0) {
        wcscpy_s(msg, 220, L"未检测到采集卡\n请检查 HDMI 连线与 USB 供电后，点右侧「刷新」");
    } else if (g_state.video_source < 0) {
        wcscpy_s(msg, 220, L"尚未选择采集源\n请在右侧「视频源」里选择你的采集卡");
    } else {
        wcscpy_s(msg, 220, L"正在打开采集源…");
    }

    /* 居中圆环 + 播放三角 */
    int r = S(26);
    int cx = (rc.left + rc.right) / 2;
    int cy = (rc.top + rc.bottom) / 2 - S(34);
    HPEN pen = CreatePen(PS_SOLID, S(2), RGB(48, 56, 70));
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
    POINT tri[3] = {{cx - S(6), cy - S(10)}, {cx - S(6), cy + S(10)}, {cx + S(10), cy}};
    HBRUSH tb = CreateSolidBrush(RGB(48, 56, 70));
    HGDIOBJ ob2 = SelectObject(dc, tb);
    HGDIOBJ op2 = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, tri, 3);
    SelectObject(dc, op2);
    SelectObject(dc, ob2);
    DeleteObject(tb);

    RECT tr = R(rc.left + S(24), cy + r + S(14), rc.right - S(24), cy + r + S(14) + S(56));
    txt(dc, tr, msg, g_fBody, C_DIM, DT_CENTER | DT_TOP | DT_WORDBREAK);
}

static void video_center(POINT *pt, int *w, int *h) {
    RECT rc;
    GetClientRect(g_video, &rc);
    *w = rc.right; *h = rc.bottom;
    pt->x = rc.right / 2;
    pt->y = rc.bottom / 2;
}

static void emit_mouse(int dx, int dy, int b0, int b1, int b2, int wheel) {
    if (!g_state.grabbed) return;
    mouse_ev_t e;
    ZeroMemory(&e, sizeof(e));
    e.dx = dx; e.dy = dy; e.wheel = wheel;
    e.buttons[0] = b0; e.buttons[1] = b1; e.buttons[2] = b2;
    input_dispatch_mouse(&e);
}

static void emit_key(WPARAM vk, bool down) {
    if (!g_state.grabbed) return;
    key_ev_t e;
    ZeroMemory(&e, sizeof(e));
    e.code = (int)vk;
    e.down = down;
    e.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    e.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    e.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    e.meta = (GetKeyState(VK_LWIN) & 0x8000) != 0;
    input_dispatch_key(&e);
}

static void set_grab(bool on) {
    g_state.grabbed = on;
    if (!on) input_clear_keys();
    if (on && !g_grabbed_cursor_hidden) {
        ShowCursor(FALSE);
        g_grabbed_cursor_hidden = true;
        SetFocus(g_video);
        POINT c; int w, h;
        video_center(&c, &w, &h);
        ClientToScreen(g_video, &c);
        SetCursorPos(c.x, c.y);
    } else if (!on && g_grabbed_cursor_hidden) {
        ShowCursor(TRUE);
        g_grabbed_cursor_hidden = false;
    }
    cmb_log("ok", on ? "已开启键鼠接管" : "已释放键鼠接管");
    InvalidateRect(g_hwnd, NULL, FALSE);
}

LRESULT CALLBACK video_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);

            /* 直接画到窗口 DC, 少一次整幅 BitBlt (1080p60 下省得可观) */
            fill_rect(dc, rc, RGB(0, 0, 0));
            if (!capture_blit(dc, &rc)) video_hint(dc, rc);

            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
            SetFocus(hwnd);
            if (g_state.grabbed) {
                POINT c; int w, h;
                video_center(&c, &w, &h);
                ClientToScreen(hwnd, &c);
                SetCursorPos(c.x, c.y);
                emit_mouse(0, 0, 1, 0, 0, 0);
            }
            return 0;
        case WM_LBUTTONUP:
            if (g_state.grabbed) emit_mouse(0, 0, -1, 0, 0, 0);
            return 0;
        case WM_RBUTTONDOWN:
            if (g_state.grabbed) emit_mouse(0, 0, 0, 1, 0, 0);
            return 0;
        case WM_RBUTTONUP:
            if (g_state.grabbed) emit_mouse(0, 0, 0, -1, 0, 0);
            return 0;
        case WM_MBUTTONDOWN:
            if (g_state.grabbed) emit_mouse(0, 0, 0, 0, 1, 0);
            return 0;
        case WM_MBUTTONUP:
            if (g_state.grabbed) emit_mouse(0, 0, 0, 0, -1, 0);
            return 0;
        case WM_MOUSEMOVE: {
            if (!g_state.grabbed) return 0;
            POINT c; int w, h;
            video_center(&c, &w, &h);
            int dx = GET_X_LPARAM(lp) - c.x;
            int dy = GET_Y_LPARAM(lp) - c.y;
            if (dx || dy) {
                POINT sp = c;
                ClientToScreen(hwnd, &sp);
                SetCursorPos(sp.x, sp.y);
                emit_mouse(dx, dy, 0, 0, 0, 0);
            }
            return 0;
        }
        case WM_MOUSEWHEEL:
            if (g_state.grabbed) emit_mouse(0, 0, 0, 0, 0, GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) { set_grab(false); return 0; }
            emit_key(wp, true);
            return 0;
        case WM_KEYUP:
            emit_key(wp, false);
            return 0;
        case WM_SETCURSOR:
            if (g_state.grabbed) { SetCursor(NULL); return TRUE; }
            break;
        case WM_KILLFOCUS:
            if (g_state.grabbed) {
                input_release_all();
                g_state.grabbed = false;
                if (g_grabbed_cursor_hidden) {
                    ShowCursor(TRUE);
                    g_grabbed_cursor_hidden = false;
                }
                InvalidateRect(g_hwnd, NULL, FALSE);
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---------- 托盘 ---------- */
static HICON make_icon(void) {
    int n = 32;
    HDC screen = GetDC(NULL);
    HDC color = CreateCompatibleDC(screen);
    HBITMAP hbColor = CreateCompatibleBitmap(screen, n, n);
    HGDIOBJ oldc = SelectObject(color, hbColor);

    RECT rc = R(0, 0, n, n);
    fill_rect(color, rc, RGB(14, 17, 22));
    fill_rr(color, R(2, 2, n - 2, n - 2), 8, RGB(76, 141, 255));
    POINT tri[3] = {{11, 8}, {11, 24}, {25, 16}};
    HBRUSH wb = CreateSolidBrush(RGB(255, 255, 255));
    HGDIOBJ ob = SelectObject(color, wb);
    HGDIOBJ op = SelectObject(color, GetStockObject(NULL_PEN));
    Polygon(color, tri, 3);
    SelectObject(color, op);
    SelectObject(color, ob);
    DeleteObject(wb);
    SelectObject(color, oldc);

    HDC mask = CreateCompatibleDC(screen);
    HBITMAP hbMask = CreateBitmap(n, n, 1, 1, NULL);
    HGDIOBJ oldm = SelectObject(mask, hbMask);
    RECT mr = R(0, 0, n, n);
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(mask, &mr, white);
    DeleteObject(white);
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    fill_rr(mask, R(2, 2, n - 2, n - 2), 8, RGB(0, 0, 0));
    HGDIOBJ ob2 = SelectObject(mask, black);
    HGDIOBJ op2 = SelectObject(mask, GetStockObject(NULL_PEN));
    Polygon(mask, tri, 3);
    SelectObject(mask, op2);
    SelectObject(mask, ob2);
    DeleteObject(black);
    SelectObject(mask, oldm);

    ICONINFO ii;
    ZeroMemory(&ii, sizeof(ii));
    ii.fIcon = TRUE;
    ii.hbmColor = hbColor;
    ii.hbmMask = hbMask;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(hbColor);
    DeleteObject(hbMask);
    DeleteDC(color);
    DeleteDC(mask);
    ReleaseDC(NULL, screen);
    return icon;
}

static void tray_add(void) {
    if (g_tray_added) return;
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_ID;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_icon ? g_icon : LoadIconW(NULL, IDI_APPLICATION);
    wcscpy_s(nid.szTip, 128, L"采集卡桥 · 采集中枢");
    if (Shell_NotifyIconW(NIM_ADD, &nid)) g_tray_added = true;
}

static void tray_remove(void) {
    if (!g_tray_added) return;
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_ID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_tray_added = false;
}

static void tray_balloon(void) {
    if (!g_tray_added) return;
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_ID;
    nid.uFlags = NIF_INFO;
    wcscpy_s(nid.szInfoTitle, 64, L"采集卡桥");
    wcscpy_s(nid.szInfo, 200, L"已最小化到托盘，双击图标可恢复窗口");
    nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void resume_capture(void) {
    /* 从托盘/最小化恢复: 重新打开预览; 驱动没放干净就 1.2s 后重试 */
    if (!g_cap_was_active) return;
    int fps = g_fps_backup > 0 ? g_fps_backup : 60;
    if (capture_start(g_state.video_source, 0, 0, fps)) {
        g_cap_was_active = false;
        g_resume_tries = 0;
        gui_request_repaint();
    } else if (g_resume_tries < 4) {
        g_resume_tries++;
        SetTimer(g_hwnd, TIMER_RESUME, 1200, NULL);
    }
}

static void hide_to_tray(void) {
    /* 托盘里没人看画面: 直接停掉采集, CPU 归零; 恢复时自动重开 */
    if (capture_is_active()) {
        g_fps_backup = g_state.fps_cap;
        g_cap_was_active = true;
        capture_stop();
    }
    ShowWindow(g_hwnd, SW_HIDE);
    g_in_tray = true;
    tray_balloon();
}

static void restore_from_tray(void) {
    ShowWindow(g_hwnd, SW_SHOW);
    ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
    g_in_tray = false;
    resume_capture();
}

/* ---------- 交互 ---------- */
static int hit_test(POINT pt) {
    if (g_popup_open) {
        RECT pr = g_popup_rect;
        if (PtInRect(&pr, pt)) {
            int idx = (pt.y - pr.top - S(4)) / S(28);
            if (pt.y >= pr.top + S(4) && idx >= 0 && idx < g_popup_count)
                return ID_POP_BASE + idx;
            return ID_POP_AREA;
        }
    }
    if (PtInRect(&g_ui.btnTray, pt)) return ID_BTN_TRAY;
    if (PtInRect(&g_ui.btnCopy, pt)) return ID_BTN_COPY;
    if (PtInRect(&g_ui.comboVideo, pt)) return ID_COMBO_VIDEO;
    if (PtInRect(&g_ui.btnRefresh, pt)) return ID_BTN_REFRESH;
    if (PtInRect(&g_ui.segSerial, pt)) return ID_SEG_SERIAL;
    if (PtInRect(&g_ui.segUdp, pt)) return ID_SEG_UDP;
    if (PtInRect(&g_ui.segHid, pt)) return ID_SEG_HID;
    if (PtInRect(&g_ui.comboSerial, pt)) return ID_COMBO_SERIAL;
    if (PtInRect(&g_ui.comboBaud, pt)) return ID_COMBO_BAUD;
    if (PtInRect(&g_ui.btnConnect, pt)) return ID_BTN_CONNECT;
    if (PtInRect(&g_ui.btnDiscover, pt)) return ID_BTN_DISCOVER;
    if (PtInRect(&g_ui.btnDisconnect, pt)) return ID_BTN_DISCONNECT;
    if (PtInRect(&g_ui.swGrab, pt)) return ID_SW_GRAB;
    if (PtInRect(&g_ui.swLan, pt)) return ID_SW_LAN;
    return ID_NONE;
}

static int read_port(void) {
    wchar_t buf[16] = L"47800";
    HWND e = GetDlgItem(g_hwnd, 900);
    if (e) GetWindowTextW(e, buf, 16);
    int p = _wtoi(buf);
    if (p < 1 || p > 65535) p = 47800;
    return p;
}

static void read_udp_fields(void) {
    wchar_t buf[128] = L"";
    HWND eh = GetDlgItem(g_hwnd, 901);
    if (eh) GetWindowTextW(eh, buf, 128);
    /* 去掉前后空白 */
    wchar_t *s = buf;
    while (*s == L' ' || *s == L'\t') s++;
    wchar_t *e = s + wcslen(s);
    while (e > s && (e[-1] == L' ' || e[-1] == L'\t')) *--e = 0;
    if (*s) {
        char tmp[64] = "";
        WideCharToMultiByte(CP_UTF8, 0, s, -1, tmp, sizeof(tmp), NULL, NULL);
        strncpy_s(g_state.udp_host, sizeof(g_state.udp_host), tmp, _TRUNCATE);
    }
    wchar_t pb[16] = L"4210";
    HWND ep = GetDlgItem(g_hwnd, 902);
    if (ep) GetWindowTextW(ep, pb, 16);
    int port = _wtoi(pb);
    if (port < 1 || port > 65535) port = 4210;
    g_state.udp_port = port;
}

static void copy_pipe(void) {
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    const wchar_t *s = L"\\\\.\\pipe\\cmb-mcp";
    size_t bytes = (wcslen(s) + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        void *p = GlobalLock(h);
        if (p) { memcpy(p, s, bytes); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT, h); }
    }
    CloseClipboard();
    cmb_log("ok", "已复制管道路径 \\.\\pipe\\cmb-mcp");
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void activate(int id) {
    if (id >= ID_POP_BASE) { popup_commit(id - ID_POP_BASE); return; }
    if (id == ID_POP_AREA) { popup_close(); return; }
    switch (id) {
        case ID_BTN_TRAY: hide_to_tray(); break;
        case ID_BTN_COPY: copy_pipe(); break;
        case ID_BTN_REFRESH:
            reload_devices();
            cmb_log("ok", "设备已刷新：采集源 %d 个 / 串口 %d 个", g_video_count, g_port_count);
            layout(g_hwnd);
            InvalidateRect(g_hwnd, NULL, FALSE);
            break;
        case ID_COMBO_VIDEO: popup_open(g_hwnd, ID_COMBO_VIDEO); break;
        case ID_COMBO_SERIAL: popup_open(g_hwnd, ID_COMBO_SERIAL); break;
        case ID_COMBO_BAUD: popup_open(g_hwnd, ID_COMBO_BAUD); break;
        case ID_SEG_SERIAL:
            if (g_state.link_state != LINK_IDLE) link_disconnect();
            g_state.channel = CH_SERIAL;
            cmb_log("ok", "通道切换为 串口 → ESP32");
            layout(g_hwnd);
            InvalidateRect(g_hwnd, NULL, FALSE);
            break;
        case ID_SEG_UDP:
            if (g_state.link_state != LINK_IDLE) link_disconnect();
            g_state.channel = CH_UDP;
            cmb_log("ok", "通道切换为 UDP → ESP32（%s:%d）",
                    g_state.udp_host, g_state.udp_port);
            layout(g_hwnd);
            InvalidateRect(g_hwnd, NULL, FALSE);
            break;
        case ID_SEG_HID:
            if (g_state.link_state != LINK_IDLE) link_disconnect();
            g_state.channel = CH_HID;
            cmb_log("ok", "通道切换为 本机注入");
            layout(g_hwnd);
            InvalidateRect(g_hwnd, NULL, FALSE);
            break;
        case ID_BTN_CONNECT: {
            if (g_state.channel == CH_SERIAL && g_state.serial_port[0] == 0 && g_port_count > 0)
                strncpy_s(g_state.serial_port, sizeof(g_state.serial_port), g_ports[0].port, _TRUNCATE);
            if (g_state.channel == CH_UDP) read_udp_fields();
            link_connect();
            InvalidateRect(g_hwnd, NULL, FALSE);
            break;
        }
        case ID_BTN_DISCOVER:
            if (g_state.udp_discovering) {
                cmb_log("warn", "已经在查找板子，请稍等");
            } else {
                cmb_log("info", "开始自动发现 AI-KVM 板子（mDNS → 广播 → 扫网段）");
                udp_auto_discover_async();
            }
            InvalidateRect(g_hwnd, NULL, FALSE);
            break;
        case ID_BTN_DISCONNECT:
            link_disconnect();
            InvalidateRect(g_hwnd, NULL, FALSE);
            break;
        case ID_SW_GRAB:
            set_grab(!g_state.grabbed);
            break;
        case ID_SW_LAN:
            if (g_state.lan_enabled) lan_mcp_stop();
            else lan_mcp_start(read_port());
            InvalidateRect(g_hwnd, NULL, FALSE);
            break;
    }
}

/* ---------- 主窗口过程 ---------- */
static void paint_window(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);

    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rw(rc), rh(rc));
    HGDIOBJ ob = SelectObject(mem, bmp);

    paint_all(mem);
    if (g_popup_open) popup_paint(mem);

    BitBlt(dc, 0, 0, rw(rc), rh(rc), mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

static void show_tray_menu(void) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"显示主界面");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, 2, L"退出程序");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                  pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(menu);

    if (cmd == 1) restore_from_tray();
    else if (cmd == 2) { tray_remove(); DestroyWindow(g_hwnd); }
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_hwnd = hwnd;
            if (g_dpi <= 0) {
                HDC sdc = GetDC(hwnd);
                g_dpi = GetDeviceCaps(sdc, LOGPIXELSX);
                ReleaseDC(hwnd, sdc);
            }
            if (g_dpi <= 0) g_dpi = 96;

            g_fTitle = CreateFontW(-S(17), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   FF_SWISS, L"Microsoft YaHei UI");
            g_fHead  = CreateFontW(-S(14), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   FF_SWISS, L"Microsoft YaHei UI");
            g_fBody  = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   FF_SWISS, L"Microsoft YaHei UI");
            g_fSmall = CreateFontW(-S(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   FF_SWISS, L"Microsoft YaHei UI");
            g_fNum   = CreateFontW(-S(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   FF_SWISS, L"Consolas");

            g_video = CreateWindowExW(0, L"CMB_VIDEO", L"", WS_CHILD | WS_VISIBLE,
                                      0, 0, 100, 100, hwnd, NULL, g_inst, NULL);

            /* 端口输入框 */
            HWND edit = CreateWindowExW(0, L"EDIT", L"47800",
                                        WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_CENTER,
                                        0, 0, 10, 10, hwnd, (HMENU)900, g_inst, NULL);
            SendMessageW(edit, WM_SETFONT, (WPARAM)g_fBody, TRUE);
            SendMessageW(edit, EM_SETLIMITTEXT, 5, 0);
            {
                wchar_t pb[16];
                swprintf_s(pb, 16, L"%d", g_state.lan_port > 0 ? g_state.lan_port : 47800);
                SetWindowTextW(edit, pb);
            }

            /* UDP 目标 IP / 端口 */
            HWND eh = CreateWindowExW(0, L"EDIT", L"",
                                      WS_CHILD | ES_AUTOHSCROLL | ES_CENTER,
                                      0, 0, 10, 10, hwnd, (HMENU)901, g_inst, NULL);
            SendMessageW(eh, WM_SETFONT, (WPARAM)g_fBody, TRUE);
            SendMessageW(eh, EM_SETLIMITTEXT, 40, 0);
            SendMessageW(eh, EM_SETCUEBANNER, TRUE, (LPARAM)L"板子 IP");
            {
                wchar_t w[64];
                MultiByteToWideChar(CP_UTF8, 0, g_state.udp_host, -1, w, 64);
                SetWindowTextW(eh, w);
            }
            HWND ep = CreateWindowExW(0, L"EDIT", L"4210",
                                      WS_CHILD | ES_NUMBER | ES_CENTER,
                                      0, 0, 10, 10, hwnd, (HMENU)902, g_inst, NULL);
            SendMessageW(ep, WM_SETFONT, (WPARAM)g_fBody, TRUE);
            SendMessageW(ep, EM_SETLIMITTEXT, 5, 0);
            SendMessageW(ep, EM_SETCUEBANNER, TRUE, (LPARAM)L"端口 4210");

            /* 让输入框跟随深色主题（主题化的 EDIT 会忽略 WM_CTLCOLOREDIT） */
            {
                HMODULE ux = LoadLibraryW(L"uxtheme.dll");
                if (ux) {
                    typedef HRESULT (WINAPI *PFN)(HWND, LPCWSTR, LPCWSTR);
                    PFN f = (PFN)(void *)GetProcAddress(ux, "SetWindowTheme");
                    if (f) {
                        f(GetDlgItem(hwnd, 900), L" ", L" ");
                        f(eh, L" ", L" ");
                        f(ep, L" ", L" ");
                    }
                }
            }

            reload_devices();
            if (g_port_count > 0 && g_state.serial_port[0] == 0)
                strncpy_s(g_state.serial_port, sizeof(g_state.serial_port), g_ports[0].port, _TRUNCATE);

            g_icon = make_icon();
            tray_add();
            SetTimer(hwnd, TIMER_UI, 400, NULL);
            return 0;
        }

        case WM_SIZE:
            if (wp == SIZE_MINIMIZED) { hide_to_tray(); return 0; }
            resume_capture();
            layout(hwnd);
            return 0;

        case WM_ACTIVATEAPP:
            if (!wp && g_state.grabbed) {
                input_release_all();
                g_state.grabbed = false;
                if (g_grabbed_cursor_hidden) {
                    ShowCursor(TRUE);
                    g_grabbed_cursor_hidden = false;
                }
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;

        case WM_GETMINMAXINFO: {
            MINMAXINFO *mm = (MINMAXINFO *)lp;
            mm->ptMinTrackSize.x = S(980);
            mm->ptMinTrackSize.y = S(680);
            return 0;
        }

        case WM_TIMER:
            if (wp == TIMER_RESUME) {
                /* 从托盘恢复时采集卡驱动可能还没放干净, 分几次重开 */
                KillTimer(hwnd, TIMER_RESUME);
                if (g_cap_was_active) resume_capture();
                return 0;
            }
            if (wp == TIMER_UI) {
                /* 通道可能被 MCP / 按钮改掉, 这里同步控件显隐 */
                layout(hwnd);
                /* 自动发现拿到 IP 后回填输入框（用户正在编辑时不动它） */
                HWND eh = GetDlgItem(hwnd, 901);
                if (eh && g_state.udp_host[0] &&
                    GetFocus() != eh &&
                    g_state.channel == CH_UDP) {
                    char cur[64] = "";
                    GetWindowTextA(eh, cur, sizeof(cur));
                    if (strcmp(cur, g_state.udp_host) != 0) {
                        wchar_t w[64];
                        MultiByteToWideChar(CP_UTF8, 0, g_state.udp_host, -1, w, 64);
                        SetWindowTextW(eh, w);
                    }
                }
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;

        case WM_PAINT:
            paint_window(hwnd);
            return 0;

        case WM_MOUSEMOVE: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (g_popup_open) {
                RECT pr = g_popup_rect;
                int idx = -1;
                if (PtInRect(&pr, pt)) {
                    int k = (pt.y - pr.top - S(4)) / S(28);
                    if (pt.y >= pr.top + S(4) && k >= 0 && k < g_popup_count) idx = k;
                }
                if (idx != g_popup_hover) {
                    g_popup_hover = idx;
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                return 0;
            }
            int id = hit_test(pt);
            if (id != g_btn_hover) {
                g_btn_hover = id;
                SetCursor(LoadCursorW(NULL, id ? IDC_HAND : IDC_ARROW));
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (g_popup_open) {
                RECT pr = g_popup_rect;
                if (PtInRect(&pr, pt)) {          /* 点在列表里, 交给 UP 处理 */
                    g_pressed = hit_test(pt);
                    SetCapture(hwnd);
                    return 0;
                }
                popup_close();                     /* 点在外面: 收起列表 */
            }
            int id = hit_test(pt);
            g_pressed = id;
            SetCapture(hwnd);
            return 0;
        }

        case WM_LBUTTONUP: {
            ReleaseCapture();
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int id = hit_test(pt);
            if (id && id == g_pressed) activate(id);
            g_pressed = 0;
            return 0;
        }

        case WM_KEYDOWN:
            if (wp == VK_ESCAPE && g_popup_open) { popup_close(); return 0; }
            break;

        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                SetCursor(LoadCursorW(NULL, hit_test(pt) ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;

        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wp;
            SetTextColor(dc, C_TEXT);
            SetBkColor(dc, C_INSET);
            static HBRUSH br = NULL;
            if (!br) br = CreateSolidBrush(C_INSET);
            return (LRESULT)br;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_TRAY:
            if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) restore_from_tray();
            else if (lp == WM_RBUTTONUP) show_tray_menu();
            return 0;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            input_release_all();
            g_state.grabbed = false;
            KillTimer(hwnd, TIMER_UI);
            tray_remove();
            if (g_icon) { DestroyIcon(g_icon); g_icon = NULL; }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---------- 对外 ---------- */
int gui_run(HINSTANCE hInst, int nCmdShow) {
    g_inst = hInst;
    SetProcessDPIAware();

    /* 先取系统 DPI, 再按比例决定窗口尺寸(否则窗口会偏小、字挤) */
    HDC sdc = GetDC(NULL);
    g_dpi = GetDeviceCaps(sdc, LOGPIXELSX);
    ReleaseDC(NULL, sdc);
    if (g_dpi <= 0) g_dpi = 96;

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hInst;
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"CMB_MAIN";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    RegisterClassExW(&wc);

    WNDCLASSEXW vc;
    ZeroMemory(&vc, sizeof(vc));
    vc.cbSize = sizeof(vc);
    vc.style = CS_HREDRAW | CS_VREDRAW;
    vc.lpfnWndProc = video_proc;
    vc.hInstance = hInst;
    vc.hbrBackground = NULL;
    vc.lpszClassName = L"CMB_VIDEO";
    vc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    RegisterClassExW(&vc);

    int w = S(1180), h = S(800);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = wa.left + (rw(wa) - w) / 2;
    int y = wa.top + (rh(wa) - h) / 2;

    g_hwnd = CreateWindowExW(0, L"CMB_MAIN", L"采集卡桥",
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             x, y, w, h, NULL, NULL, hInst, NULL);
    if (!g_hwnd) return 1;

    layout(g_hwnd);
    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);
    cmb_log("ok", "界面就绪 DPI=%d 逻辑 1180x800", g_dpi);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

void gui_request_repaint(void) {
    if (g_video) InvalidateRect(g_video, NULL, FALSE);
    /* 画面统计（fps/缓冲/帧数）与向导卡点最多每 400ms 刷一次, 免得 60fps 下整窗重绘 */
    static double last_hdr = 0;
    double t = now_sec();
    if (t - last_hdr >= 0.4 && g_hwnd) {
        last_hdr = t;
        RECT hdr = g_ui.videoCard;
        hdr.bottom = hdr.top + S(36);
        InvalidateRect(g_hwnd, &hdr, FALSE);
        InvalidateRect(g_hwnd, &g_ui.guideCard, FALSE);
    }
}

void gui_show_message(const char *text) {
    wchar_t w[512];
    MultiByteToWideChar(CP_UTF8, 0, text, -1, w, 512);
    MessageBoxW(g_hwnd, w, L"采集卡桥", MB_OK | MB_ICONINFORMATION);
}
