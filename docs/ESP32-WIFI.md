# ESP32 Wi-Fi / 热点指南

## 默认启动

固件会先读取 ESP32 NVS：

- 有 STA SSID：尝试连接保存的路由器。
- 连接超时：保持 STA 离线，不自动切换网络。
- 没有凭据：直接启动 AP，作为首次配网入口。

AP 名称为 `AI-KVM-` 加 MAC 后六位，默认密码为 `RM-KVM-` 加 MAC 后六位。密码不会写入源码或仓库。

连接到热点后访问 `http://<板子IP>/` 可以查看状态。STA 已连接时，状态页右上角的“使用 AP 模式”按钮会持久化 AP 选择并重启；AP 页面会显示“切回 STA 模式”。桌面端会通过 `ai-kvm.local`、UDP 广播和网段发现自动寻找板子。

## 配网

通过 USB 串口发送以下命令，命令行以换行结束：

```text
WIFI,STA,my-network,my-password
WIFI,AP,my-kvm-ap,my-ap-password
WIFI,STATUS
WIFI,OFF
```

STA/AP 配置写入 NVS，重启后自动恢复。密码只存在板端 NVS 和串口传输中，禁止把真实密码写入 issue、日志或 Git。

## UDP HID

UDP 端口固定为 4210，单包 16 字节：

| 字节 | 内容 |
| --- | --- |
| 0 | 设备类型：键盘 `0x01`、鼠标 `0x02`、控制 `0x03` |
| 1 | 键盘修饰位或控制命令 |
| 2..7 | 6 个 HID usage |
| 8..9 | 鼠标 dx，小端 int16 |
| 10..11 | 鼠标 dy，小端 int16 |
| 12 | 鼠标按钮掩码 |
| 13 | 滚轮 int8 |
| 14..15 | 序号，小端 uint16 |

控制命令：`1` ping、`4` 设置节奏、`5` release all、`6` status。重复序号会被丢弃；超过 2 秒没有任何输入时，固件会释放键盘和鼠标按钮，避免上位机异常退出造成卡键。

## 故障排查

1. 热点可见但 UDP 不通：先访问 `/api/status`，确认 `udp_port`、`ip` 和 `hid_mounted`。
2. UDP 有计数但被控机无输入：确认 ESP32 原生 USB 口接在被控机，而不是刷机口；确认 USB HID/OTG 选项正确。
3. STA 连接失败：发送 `WIFI,STATUS` 查看是否已回退 AP，再发送新的 `WIFI,STA,...` 配置。
4. 需要恢复安全状态：发送 `WIFI,OFF` 后重新上电，固件会在没有 STA 凭据时恢复默认 AP。
