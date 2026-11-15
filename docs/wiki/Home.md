# CaptureMouseBridge Wiki

CaptureMouseBridge 把 HDMI/UVC 采集、原生 Win32 预览、ESP32 USB HID 和 MCP 自动化接口拆成独立数据面。采集卡阻塞不会阻塞键鼠链路，ESP32 断线也不会拖垮视频线程。

## 快速入口

- [编译与运行](../../README.md#编译)
- [MCP 接口](../MCP.md)
- [ESP32 Wi-Fi 与热点](../ESP32-WIFI.md)
- [UDP 协议](../../README.md#udp-协议对接-ai-kvm-板子默认通道)
- [发布检查](../release-checklist.md)

## 设计原则

1. **安全释放**：输入空闲超时、链路断开和程序退出都会发送 release all。
2. **最新帧优先**：视频缓冲丢弃积压帧，键鼠事件不等待视频帧。
3. **协议稳定**：MCP 使用 JSON-RPC 2.0；UDP 维持 16 字节兼容报文；固件同时保留串口恢复入口。
4. **可验证发布**：仓库只发布源代码、协议、文档和测试，不发布密钥、私有 IP、模型权重或构建产物。

## 贡献方式

提交前运行原生构建、协议测试和 `scripts/redact_scan.py . --public`。新接口必须同时更新 MCP 表、协议文档和至少一个可重复测试。
