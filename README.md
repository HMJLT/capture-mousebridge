# CaptureMouseBridge（采集卡桥）

[![MIT License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)
[![ESP32](https://img.shields.io/badge/firmware-ESP32--S2%2FS3-blue.svg)](firmware/esp32_cmb_firmware.ino)
[![MCP](https://img.shields.io/badge/MCP-JSON--RPC-orange.svg)](docs/MCP.md)

采集卡画面实时预览 + 键鼠透传（类虚拟机映射）+ ESP32 断线重连向导 + 内置 MCP 的纯 C / MSVC 原生桌面程序，编译为单个 `CaptureMouseBridge.exe`（无 Python、无浏览器面板、默认不占端口）。

- **原生 GUI**：Win32 自绘窗口（Unicode / 深色卡片式），支持最小化到系统托盘。
- **采集走 Media Foundation**：直接吃 UVC 采集卡，**不再走 avicap/VFW**——不会弹驱动自己的「视频源选择」对话框，也不会只拿到黑帧。
- **键鼠透传（双链路）**：画面区接管鼠标/键盘，走 **Wi-Fi UDP（默认，16 字节 HID 报文，端口 4210）** 或串口发给 ESP32，由固件模拟 USB HID；也能「本机注入」自测。
- **MCP 双通道**：本机命名管道 `\\.\pipe\cmb-mcp` 常开；回环 TCP 可选开启（默认关）。

## 主要功能

| 模块 | 说明 |
| --- | --- |
| 实时预览 | Media Foundation `IMFSourceReader` 拉 RGB32 帧，直接写双缓冲 DIB Section，临界区换指针，不整帧拷贝；按目标帧率丢帧省 CPU |
| 键鼠映射 | 画面区捕获移动/按键/滚轮/键盘；`udp` 通道发 16 字节 HID 报文给板子（默认），`serial` 通道走串口行协议，`hid` 通道本机 `SendInput` 注入；`Esc` 释放接管 |
| 自动发现板子 | 启动即在后台按 **mDNS `ai-kvm.local` → 同网段广播 → 扫本机各 /24** 三级查找 ESP32，找到自动回填 IP 并连上；也可点「自动发现」手动触发 |
| 断线重连向导 | 五步：采集卡 → ESP32 上电 → 串口波特率 → 心跳 → 键鼠接管；绿点=已通过，并标出「当前卡点」；掉线后按间隔自动重连 |
| MCP | JSON-RPC 2.0 文本帧（`\n` 结尾）。`status` 一条命令拿全量压缩状态，适合 Agent 低 token 轮询 |

## 界面

窗口逻辑尺寸 1180×800（按系统 DPI 缩放），左栏实时预览 + 重连向导，右栏视频源 / 链路 / 键鼠接管 / MCP 接口 / 事件日志。

- 顶部：应用标题、连接状态胶囊、「最小化到托盘」。
- 关闭按钮 = 退出程序；托盘菜单含「显示主界面」「退出程序」，左键单击托盘图标恢复窗口。
- 下拉选择（视频源 / 串口 / 波特率）在主窗口内绘制，不弹独立窗口。

## 目录

```
capture-mousebridge/
├─ src/
│  ├─ main.c            WinMain 入口 / 命令行参数 / 后端轮询线程
│  ├─ common.h/.c       全局状态、日志环形缓冲、字符串/JSON 工具
│  ├─ capture.h/.c      Media Foundation 采集 + 双缓冲 DIB
│  ├─ serial_link.h/.c  Win32 串口枚举/收发
│  ├─ input_bridge.h/.c 键鼠事件映射、SendInput 注入
│  ├─ udp_link.h/.c      Wi-Fi UDP 通道（16 字节 HID 报文、状态回包、自动发现）
│  ├─ link.h/.c         链路状态机、自动重连
│  ├─ mcp.h/.c          MCP 工具分发与 JSON 输出
│  ├─ pipe_mcp.h/.c     命名管道 MCP（本机常开）
│  ├─ lan_mcp.h/.c      局域网 TCP MCP（可选，默认关）
│  └─ gui.h/.c          Win32 自绘界面、托盘、视频子窗口
├─ firmware/
│  └─ esp32_cmb_firmware.ino  ESP32 USB HID 固件示例
├─ build/
│  ├─ write_bat.py      生成 build.bat（CRLF、无 BOM）
│  ├─ ui_test.py        自动化验收（点击/托盘/MCP/TCP）
│  └─ CaptureMouseBridge.exe
└─ build.bat            一键编译（MSVC + Windows SDK，不依赖 vcvars）
```

## 编译

环境：Visual Studio（安装 Desktop development with C++ 和 Windows 10/11 SDK）。`build.bat` 通过 `vswhere` 自动寻找 MSVC，不绑定某台机器的安装路径。

```
build.bat
```

产物 `build\CaptureMouseBridge.exe`（`/SUBSYSTEM:WINDOWS`，无控制台）。链接库：`mf.lib mfplat.lib mfreadwrite.lib mfuuid.lib mfcore.lib ole32.lib user32.lib gdi32.lib advapi32.lib ws2_32.lib shell32.lib`。

## 运行

```
CaptureMouseBridge.exe [--source N] [--serial COM3] [--baud 115200]
                       [--channel udp|serial|hid] [--udp-host IP]
                       [--udp-port 4210] [--udp-interval 12]
                       [--lan] [--lan-port 47800]
```

| 参数 | 说明 | 默认 |
| --- | --- | --- |
| `--source N` | 采集源序号；`-1` = 启动不自动开，界面里再选 | -1 |
| `--serial PORT` | ESP32 串口，如 `COM3` | 空 |
| `--baud N` | 串口波特率 | 115200 |
| `--channel udp\|serial\|hid` | 透传通道 | udp |
| `--udp-host IP` | 板子 IP；留空时启动自动发现并回填 | 空（自动发现） |
| `--udp-port N` | 板子 UDP 端口 | 4210 |
| `--udp-interval N` | 鼠标合并发送间隔（ms），越小越跟手 | 4 |
| `--fps N` | 预览目标帧率；1080p60 吃 CPU，弱机可设 30 | 60 |
| `--lan` | 启动即开启回环 TCP MCP | 关 |
| `--lan-port N` | 局域网 MCP 端口 | 47800 |

### 上手步骤

1. 采集卡接 HDMI + USB，打开程序 → 右侧「刷新」→「视频源」下拉选卡（如 `USB Video`）。画面区显示「宽×高 · fps」与已接收帧数。
2. 板子插到被控端原生 USB 口、连上同一个 Wi-Fi。切到「UDP」分段：启动时会自动发现板子并回填地址；没找到就点「自动发现」，或直接在 IP/端口框里手填。
3. 点「连接」→ 打开「接管被控端鼠标键盘」，在画面区操作即转发；`Esc` 或再点一次开关释放。
4. 底部「ESP32 断线重连向导」实时标出卡点（UDP 模式第 3 步是「UDP 地址」），板子 6 秒无回包判定掉线并按间隔自动重连。

### 延迟与性能（实测）

| 环节 | 做法 | 实测 |
| --- | --- | --- |
| 鼠标发送 | 输入事件到达即唤醒发送线程（`SetEvent`），不再死等节拍；默认合并间隔 4ms | 管道调用到板子收包 median **1.3ms**（含 MCP 往返） |
| 板子 HID 节奏 | 连上后自动下发 `CTRL_SET_INTERVAL(4)`，固件 `pace_ms` 由 12 降到 4 | 板子回 `pace_ms: 4, queue: 0` |
| 画面延迟 | Reader 设 `MF_LOW_LATENCY`；超过 120ms 的积压帧直接读空丢弃，只显示最新帧 | 缓冲区回落到 **1~3ms**，预览 50~60fps |
| 渲染 | 直接画到窗口 DC（去掉了每帧一次的整幅 BitBlt），统计行 400ms 刷一次 | 1080p60 下 CPU 由 ~95% 降到 ~86%（单核口径） |
| 关源/换源 | `capture_stop` 先 `Shutdown()` 媒体源解锁阻塞的 `ReadSample` | 关掉再立刻重开不再报「创建读取器失败」 |

如果机器偏弱、想更省 CPU：`config_set {"fps_cap":30}`，或启动加 `--fps 30`（帧率减半、CPU 近似减半，键鼠延迟不受影响）。
### 采集故障提示

- 有设备但 5 秒没画面 → 画面区直接给出「请确认 HDMI 有信号、采集卡插稳后重新选择视频源」，不会一直停在"正在打开…"。
- 设备被其它程序（如 PotPlayer / OBS）占用 → 「无法打开采集源：可能被其它程序占用」。同一张卡不能同时被两个程序独占。

### HDMI 采集卡 + ESP32 直通链路

这套工作方式把 HDMI 采集卡和 ESP32 分成两个独立数据面。采集卡只负责把被控设备的画面送入本机，ESP32 通过 USB HID 把键鼠事件送回被控设备。画面采集异常不会阻塞键鼠链路，ESP32 断线也不会让采集线程退出。

推荐顺序是先接 HDMI 与 USB 采集卡，再让 ESP32 接入被控设备的原生 USB 口，最后在工作台中选择视频源并连接 UDP 或串口通道。调试时可用 `video_open` 和 `status` 检查画面帧数、采集分辨率、ESP32 心跳和 HID 挂载状态。上层比赛模式可以关闭工作台的 MCP 与预览，只保留已选输入源和 ESP32 控制链路。

## MCP

### 本机（默认开启）

- 管道名：`\\.\pipe\cmb-mcp`（界面里可一键「复制」）。
- 帧格式：一行一条 JSON-RPC 2.0，请求响应均以 `\n` 结尾。
- 请求：`{"tool":"status","args":{...},"id":1}`；响应：`{"jsonrpc":"2.0","id":1,"result":{...}}`。

### 局域网（可选）

- 界面「局域网访问」开关 + 端口输入框；开启后默认在 `127.0.0.1:端口` 监听，协议与本机管道完全相同，每行一条 JSON-RPC。请求行限制为 8 KiB，空闲连接在 1 秒后继续检查并可安全关闭。
- 默认关闭；关闭时本程序不占用任何 TCP 端口。端口被占用会在 MCP 卡里显示「端口 N 被占用」。
- 当前**未做 token 鉴权**，监听地址固定为回环地址。

### 工具清单

| 工具 | 作用 | 主要参数 |
| --- | --- | --- |
| `status` | 链路 / 视频 / 键鼠 / UDP / MCP 压缩摘要（含 `udp.host/port/tx/rx/hid_mounted`） | — |
| `video_devices` | 枚举采集卡/摄像头 | — |
| `serial_ports` | 枚举串口 | — |
| `connect` | 连接 ESP32（UDP 或串口） | `channel`(0串口/1本机/2 UDP),`serial_port`,`serial_baud`,`udp_host`,`udp_port` |
| `discover` | 自动发现板子 IP（mDNS/广播/扫网段），命中的 IP 会写回配置 | — |
| `video_open` | 打开采集源（省得点界面） | `index`,`w`,`h`,`fps` |
| `video_close` | 关闭采集源 | — |
| `send_text` | 按 ASCII 字符串批量敲键盘（Ctrl+Alt+T 开终端后可整行命令注入） | `text` |
| `snapshot` | 把当前帧按原分辨率存 BMP，agent 不用整屏截图就能读被控端文字 | `path` |
| `disconnect` | 断开链路 | — |
| `reconnect_guide` | 五步向导 + 当前卡点 | — |
| `set_grab` | 开关键鼠接管 | `grabbed` |
| `send_mouse` | 鼠标事件 | `dx,dy,b0,b1,b2,wheel,absolute,x,y` |
| `send_key` | 键盘事件 | `code_name,down,ctrl,shift,alt,meta` |
| `logs` | 最近日志 | `limit` |
| `config_get` | 读配置 | — |
| `config_set` | 热更新配置子集 | 标量键值 |

### 调用示例（Python）

```python
import ctypes, ctypes.wintypes as wt, json
k = ctypes.windll.kernel32

def call(payload, pipe=r"\\.\pipe\cmb-mcp"):
    h = k.CreateFileW(pipe, 0x80000000 | 0x40000000, 0, None, 3, 0, None)
    data = (json.dumps(payload) + "\n").encode()
    w = wt.DWORD(0)
    k.WriteFile(h, data, len(data), ctypes.byref(w), None)
    out = b""
    while b"\n" not in out:
        buf = ctypes.create_string_buffer(4096); r = wt.DWORD(0)
        k.ReadFile(h, buf, 4095, ctypes.byref(r), None)
        out += buf.raw[:r.value]
    k.CloseHandle(h)
    return json.loads(out.decode("utf-8"))["result"]

print(call({"tool": "status", "id": 1}))
print(call({"tool": "reconnect_guide", "id": 2}))
```

TCP 侧换成 `socket.create_connection(("127.0.0.1", 47800))`，发送同样的一行 JSON 即可。

## UDP 协议（对接 AI-KVM 板子，默认通道）

端口 `4210`，每包 16 字节，逐字节对齐板子的 `kvm_protocol`：

| 偏移 | 长度 | 含义 |
| --- | --- | --- |
| 0 | 1 | 设备：`0x01` 键盘 / `0x02` 鼠标 / `0x03` 控制 |
| 1 | 1 | 键盘修饰位（Lctrl1 Lshift2 Lalt4 Lgui8 Rctrl16 Rshift32 Ralt64 Rgui128）/ 控制子命令 |
| 2..7 | 6 | 键盘 HID usage（6-key rollover） |
| 8..9 | 2 | dx，int16 小端 |
| 10..11 | 2 | dy，int16 小端 |
| 12 | 1 | 鼠标按键掩码：bit0 左 / bit1 右 / bit2 中 |
| 13 | 1 | 滚轮 int8（一格 = ±1） |
| 14..15 | 2 | 序号 uint16 小端（自增、跳过 0，固件据此去重） |

- 控制子命令：`1` ping / `4` set_interval / `5` release_all / `6` status（固件回一行 JSON，含 `phase`/`ip`/`hid_mounted`/`rx`）。
- 鼠标位移超过 ±127 自动拆包（127 + 127 + 余数），不丢精度；事件在 12ms 定时线程里合并发送，抖动小于串口。
- 键盘维护完整 HID 报告：修饰键按下就置位，普通键最多同时 6 个，松开自动从报告里移除。

## 自动发现

优先级从高到低，全部命中即停：

1. **mDNS**：`getaddrinfo("ai-kvm.local")`，命中后发 `CTRL_STATUS` 确认（实测 2 秒内命中）。
2. **广播**：向 `255.255.255.255:4210` 发 STATUS，收应答取源地址。
3. **扫网段**：枚举本机各私有网卡的 `/24`，两轮连发 STATUS（共约 500 包），再统一收包。

全部在后台线程跑，不阻塞界面；找到后自动回填 IP 并在 UDP 通道下自动建立链路。也可以随时点「自动发现」重跑，或通过 MCP `discover` 触发。

## 串口协议（上位机 → ESP32）

行式 ASCII，行尾 `\n`：

```
M,dx,dy,l,r,s,w     鼠标相对移动; l/r/s 左/右/中键 0/1; w 滚轮增量
M,x,y,1             鼠标绝对定位 (0..1000)
K,down,code,mods    键盘按下; code=Windows VK; mods 位掩码 ctrl1/shift2/alt4/meta8
K,up,code,mods      键盘松开
P,ping              心跳, 固件回 P,pong
```

## ESP32 固件与热点

`firmware/esp32_cmb_firmware.ino` 现在是完整的公开固件：串口行协议、USB HID、Wi-Fi STA、自动 AP fallback、mDNS、UDP 4210 和 HTTP 状态页在同一份 sketch 中实现。上位机不再依赖私有厂商固件。

启动流程：

1. 若 NVS 中存在 STA 凭据，固件尝试连接保存的 Wi-Fi；连接失败或没有凭据时自动创建 `AI-KVM-<MAC后6位>` 热点。
2. 热点密码默认是 `RM-KVM-<MAC后6位>`，首次连接后可通过串口配置为自定义 SSID/密码。
3. STA/AP 两种模式都提供 `ai-kvm.local`（若网络支持 mDNS）、UDP 4210 和 HTTP 80 的 `/api/status`。

串口配置命令：

```text
WIFI,STA,<ssid>,<password>
WIFI,AP,<ssid>,<password>
WIFI,STATUS
WIFI,OFF
```

Arduino IDE 选择 ESP32-S2/S3，并启用 USB-OTG/HID。烧录后可直接打开热点地址的 `http://<板子IP>/` 查看状态；桌面端 UDP 通道会自动通过 mDNS、广播和网段发现连接。

协议字段、MCP 工具、热点状态和故障恢复步骤见 [MCP 接口说明](docs/MCP.md)、[ESP32 Wi-Fi 指南](docs/ESP32-WIFI.md) 和 [Wiki 首页](docs/wiki/Home.md)。

## 已知限制

- 同一张采集卡不能被两个程序同时占用（PotPlayer / OBS 先关掉再预览）。
- TCP MCP 无 token 鉴权，但默认只监听回环地址。
- `hid` 通道是本机 `SendInput` 注入，仅用于本机回环调试；跨机控制走 `udp`（Wi-Fi）或 `serial` + ESP32。
- 绝对定位只在串口协议里有（`M,x,y,1`）；UDP 协议只有相对位移，厂商固件按被控端 HID 上报。
- 板子 `status` 里 `hid_mounted:false` 时，键鼠包能收到但不会落到被控端：检查原生 USB 口是否插在被控机的 USB 口（不是刷机口 `CPE`）。
- 绝对定位按 0..1000 归一化发送，固件侧按被控端分辨率换算。
- 采集输出固定 RGB32（BGRA 忽略 alpha），不做硬件解码/HDR 转换。
