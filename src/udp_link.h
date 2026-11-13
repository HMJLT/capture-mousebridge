/* udp_link.h - Wi-Fi UDP 通道 (对接 AI-KVM 板子的 16 字节 HID 报文) */
#ifndef CMB_UDP_LINK_H
#define CMB_UDP_LINK_H
#include "common.h"

bool udp_open(const char *host, int port, int interval_ms);
void udp_close(void);
bool udp_is_open(void);
const char *udp_last_error(void);

/* 鼠标: dx/dy 累加后由定时线程合并发送; buttons 传 -1 表示不更新按键状态 */
bool udp_send_mouse(int dx, int dy, int buttons, int wheel);
/* 键盘: mods = HID 修饰位 (Lctrl1 Lshift2 Lalt4 Lgui8 Rctrl16 Rshift32 Ralt64 Rgui128) */
bool udp_send_keyboard(int mods, const unsigned char *keys, int nkeys);
bool udp_send_release_all(void);
/* 收状态回包 + 心跳, 由后端线程周期调用 */
void udp_poll(void);

/* 自动发现: mDNS -> 广播 -> 本机各 /24 扫描, 找到就写回 g_state.udp_host。
 * 后台线程跑, 不阻塞界面; 找到后 udp_take_discovery() 返回一次 true。 */
void udp_auto_discover_async(void);
bool udp_take_discovery(void);
/* 同步版, 供 MCP / 命令行立即拿结果; 返回找到的 IP, 失败返回 false */
bool udp_discover_now(char *ip_out, int cap, int *hid_out, char *json_out, int json_cap);

#endif
