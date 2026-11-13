/* common.h - CaptureMouseBridge 公共定义 */
#ifndef CMB_COMMON_H
#define CMB_COMMON_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

#define CMB_VERSION        "2.0.0"
#define CMB_MAX_PORTS      32
#define CMB_MAX_PATH       260
#define CMB_LOG_LIMIT      300
#define CMB_MAX_LINE       512
#define CMB_MAX_JSON       65536

/* 透传通道 */
typedef enum {
    CH_SERIAL = 0,   /* Win32 串口 -> ESP32 */
    CH_HID    = 1,   /* 本机 SendInput 注入 */
    CH_UDP    = 2    /* Wi-Fi UDP -> ESP32 (16 字节 HID 报文) */
} channel_t;

/* 链路状态机 */
typedef enum {
    LINK_IDLE = 0,
    LINK_CONNECTING,
    LINK_CONNECTED,
    LINK_DISCONNECTED,
    LINK_FAILED
} link_state_t;

typedef struct {
    char   port[CMB_MAX_PATH];
    char   description[256];
    uint16_t vid;
    uint16_t pid;
    bool   has_hwid;
} serial_port_t;

typedef struct {
    char   name[96];
    int    index;
} video_dev_t;

typedef struct {
    /* 配置 */
    int      video_source;       /* -1 = 未选择设备 */
    int      video_width;
    int      video_height;
    int      fps_cap;
    channel_t channel;
    char     serial_port[CMB_MAX_PATH];
    int      serial_baud;
    char     udp_host[64];       /* AI-KVM 板子 IP */
    int      udp_port;           /* 默认 4210 */
    int      udp_interval_ms;    /* 鼠标发包间隔, 默认 4ms */
    bool     capture_enabled;
    bool     grabbed;            /* 键鼠接管 */
    bool     auto_reconnect;
    int      reconnect_interval;

    /* 运行状态 */
    link_state_t link_state;
    char     link_device[CMB_MAX_PATH];
    int      link_baud;
    char     link_error[256];
    double   last_ok_at;
    int      reconnect_attempts;

    bool     capturing;
    int      cap_width;
    int      cap_height;
    double   cap_fps;
    int      cap_latency_ms;     /* 帧时间戳滞后于本机时钟的毫秒数, 越小越跟手 */
    long     cap_frames;
    double   last_frame_at;
    char     cap_error[256];

    long     events_sent;
    long     events_dropped;
    double   last_event_at;
    int      keys_pressed[16];
    int      key_count;

    /* UDP 通道状态 */
    int      udp_tx;
    int      udp_rx;
    int      udp_hid;            /* 固件回报 HID 是否挂到被控端 */
    bool     udp_discovering;    /* 正在后台自动发现 */
    double   udp_last_rx_at;
    char     udp_reply[96];      /* 最近一次状态回包摘要 */

    /* MCP: 本机命名管道 + 可选局域网 */
    char     mcp_endpoint[160];
    bool     lan_enabled;
    int      lan_port;
    int      lan_state;          /* 0 关闭 / 1 监听中 / 2 出错 */
    int      lan_clients;
    char     lan_addr[64];
    char     lan_error[160];

    double   boot_time;
} cmb_state_t;

extern cmb_state_t g_state;

/* 日志环形缓冲 */
typedef struct {
    double  t;
    char    level[12];
    char    msg[176];
} log_entry_t;

extern volatile LONG g_log_seq;

void cmb_log(const char *level, const char *fmt, ...);
int  cmb_log_snapshot(char *out, int cap);
/* 最近 max_lines 条, 纯文本, 每行 "HH:MM:SS [level] msg"; 返回写入字节数 */
int  cmb_log_tail_text(char *out, int cap, int max_lines);

double now_sec(void);
void   time_str(double t, char *out, int cap);

/* 协议行编码 */
void proto_mouse(char *out, int cap, int dx, int dy, const int buttons[3], int wheel,
                 bool absolute, int x, int y);
void proto_key(char *out, int cap, bool down, int code, int mods);

#endif
