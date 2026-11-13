/* mcp.h - 内置 MCP (JSON-RPC over HTTP) */
#ifndef CMB_MCP_H
#define CMB_MCP_H
#include "common.h"

/* 解析 JSON-RPC 请求并把结果写入 out (JSON 文本) */
void mcp_handle(const char *body, char *out, int cap);

/* 工具清单 (低 token 描述) */
void mcp_tool_list(char *out, int cap);

/* 供 HTTP 层复用的序列化包装 */
void write_status_pub(char *out, int cap);
void write_guide_pub(char *out, int cap);
void write_serial_ports_pub(char *out, int cap);
void write_video_devices_pub(char *out, int cap);

#endif
