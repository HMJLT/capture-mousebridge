# MCP 接口

CaptureMouseBridge 提供两个完全相同的 JSON-RPC 2.0 入口：Windows 命名管道 `\\.\pipe\cmb-mcp` 和可选的本机 TCP 端口。每行一个请求，每行一个响应；响应不会跨行，也不会返回未转义的日志文本。

## 请求格式

```json
{"tool":"status","args":{},"id":1}
```

常用工具：

| 工具 | 作用 |
| --- | --- |
| `status` | 返回采集卡、UDP、USB HID、MCP 和重连状态 |
| `video_devices` | 枚举 UVC 采集卡 |
| `video_open` / `video_close` | 打开或关闭采集源 |
| `discover` | mDNS、广播和网段发现 ESP32 |
| `connect` / `disconnect` / `reconnect_guide` | 管理 ESP32 链路 |
| `set_grab` | 开关键鼠接管 |
| `send_mouse` / `send_key` / `send_text` | 发送键鼠输入 |
| `snapshot` | 保存当前采集帧 |
| `config_get` / `config_set` | 读取或热更新安全配置 |
| `logs` | 读取环形事件日志 |

## 返回格式

成功：

```json
{"jsonrpc":"2.0","id":1,"result":{"ok":true}}
```

错误：

```json
{"jsonrpc":"2.0","id":1,"error":{"code":-32602,"message":"invalid arguments"}}
```

所有入口都限制单请求大小、客户端数量和空闲时间。局域网 MCP 默认关闭并仅监听回环地址；如果要开放到局域网，应在受控网络中使用，并在外层防火墙限制来源。

## ESP32 状态

ESP32 的 HTTP `GET /api/status` 和 UDP `CTRL_STATUS` 返回同一组核心字段：

```json
{
  "ok": true,
  "mode": "ap",
  "ssid": "AI-KVM-ABC123",
  "ip": "<board-ip>",
  "mdns": "ai-kvm.local",
  "udp_port": 4210,
  "http_port": 80,
  "rx": 42,
  "udp_rx": 40,
  "last_sequence": 17,
  "pace_ms": 4,
  "hid_mounted": true
}
```

示例中的地址是占位符，不代表固定部署地址。
