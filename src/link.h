/* link.h - 链路连接管理与自动重连 */
#ifndef CMB_LINK_H
#define CMB_LINK_H
#include "common.h"

bool link_connect(void);
void link_disconnect(void);
void link_poll(void);          /* 周期: 心跳/断线检测/自动重连 */
void link_start_reconnect_timer(void);

#endif
