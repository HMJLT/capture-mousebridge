/* pipe_mcp.h - 命名管道 MCP (无 TCP 端口) */
#ifndef CMB_PIPE_MCP_H
#define CMB_PIPE_MCP_H
#include "common.h"

/* 启动后台管道服务; 管道名 \\.\pipe\cmb-mcp */
void pipe_mcp_start(void);
void pipe_mcp_stop(void);
const char *pipe_mcp_name(void);

#endif
