/* lan_mcp.h - 可选局域网 MCP (默认关闭) */
#ifndef CMB_LAN_MCP_H
#define CMB_LAN_MCP_H
#include "common.h"

bool lan_mcp_start(int port);     /* bind 127.0.0.1:port */
void lan_mcp_stop(void);
bool lan_mcp_running(void);

#endif
