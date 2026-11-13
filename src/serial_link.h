/* serial_link.h - ESP32 串口链路 */
#ifndef CMB_SERIAL_H
#define CMB_SERIAL_H
#include "common.h"

int  serial_enumerate(serial_port_t *out, int max);
bool serial_open(const char *port, int baud);
void serial_close(void);
bool serial_is_open(void);
bool serial_send_line(const char *line);
void serial_poll_rx(void);   /* 在主线程周期调用, 处理回包 */
const char *serial_last_error(void);

#endif
