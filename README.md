# CCLoader WebUI 操作教程

> 把 NodeMCU ESP8266 升级为 **CC2530 烧录 + 监控一体机**。浏览器访问 ESP8266 的 IP，即可上传 BIN、一键烧录、实时查看 CC2530 串口日志，全程无需拔插任何线。

---

## 目录

1. [功能特性](#1-功能特性)
2. [硬件准备](#2-硬件准备)
3. [接线](#3-接线)
4. [首次部署](#4-首次部署)
5. [浏览器操作流程](#5-浏览器操作流程)
6. [工作原理](#6-工作原理)
7. [故障排查](#7-故障排查)
8. [常见问题](#8-常见问题)

---

## 1. 功能特性

| 特性 | 说明 |
|---|---|
| 跨平台 | Win/Mac/Linux/手机浏览器均可，不再依赖 `CCLoader.exe` |
| 一键烧录 | 上传 BIN → 选文件 → 点"开始烧录"，实时进度条 |
| **HEX 直传** | 上传 `.hex` 文件时浏览器自动转换为 BIN（256KB 填充），无需 Python 工具 |
| 实时监控 | 烧录完自动跳到监控页，按 CC2530 RESET 即可看启动日志 |
| 无需拔线 | 烧录与监控共用同一套接线，状态机保证互斥 |
| **智能配网** | AP 模式开放无密码，手机连上后访问任意 URL 自动弹出配网页；扫描列表点选，连接成功自动切换 STA |
| 固件管理 | LittleFS 中可保存多个 BIN，可删除可重选 |
| WiFi 配置 | 连接失败自动回退 AP 模式，跨 WiFi 切换无需重启 |
| 无外部库依赖 | 仅用 ESP8266 Core 自带的 `ESP8266WebServer` + `WiFiServer` |

---

## 2. 硬件准备

| 元件 | 说明 |
|---|---|
| NodeMCU ESP8266 (ESP-12E) | 主控，4MB Flash |
| CC2530 模块（带 Zigbee 固件下载口） | 目标芯片 |
| 杜邦线 4 根 + 共地 1 根 | 见下方接线表 |
| Micro-USB 数据线 | 给 NodeMCU 供电并烧录固件 |
| 电脑 | 已装 PlatformIO（Core 或 VSCode 插件） |

---

## 3. 接线

### 3.1 引脚对照表

| ESP8266 GPIO | NodeMCU 丝印 | 方向 | CC2530 引脚 | 用途 |
|---|---|---|---|---|
| GPIO5  | D1  | → | Pin 7 (RESETn)      | CC Debug 复位 |
| GPIO4  | D2  | → | Pin 3 (DC)          | CC Debug 时钟 |
| GPIO12 | D6  | ↔ | Pin 4 (DD)          | CC Debug 数据（双向） |
| GPIO3  | RX  | ← | P0_3 (UART0 TX)     | **监控用**，接收 CC2530 串口日志 |
| GND    | GND | — | GND                 | 共地（必须） |

> CC2530 模块的 3.3V 电源可从 NodeMCU 的 `3V3` 引脚取，电流 < 50mA。

### 3.2 接线示意图

```
   NodeMCU ESP8266                     CC2530 模块
  ┌────────────────┐                ┌─────────────────┐
  │            3V3 ├───────────────►│ VCC             │
  │            GND │◄──────────────►│ GND             │
  │  D1 (GPIO5)    ├───────────────►│ Pin 7 RESETn    │
  │  D2 (GPIO4)    ├───────────────►│ Pin 3 DC        │
  │  D6 (GPIO12)   │◄──────────────►│ Pin 4 DD        │
  │  RX (GPIO3)    │◄───────────────│ P0_3 UART0 TX   │  ← 监控用（新增第 4 根线）
  └────────────────┘                └─────────────────┘
```

### 3.3 GPIO3 (RX) 复用说明

`GPIO3` 是 ESP8266 硬件 UART0 的 RX，与 USB-TTL 的 TX 共引脚：

- **空闲/烧录态**：Serial 默认 115200，可输出 ESP8266 启动日志到 USB 串口监视器
- **监控态**：固件 `Serial.end()` → `Serial.begin(CC2530 波特率)`，GPIO3 直接接收 CC2530 P0_3 的串口日志

由于 WebUI 方案下上位机通过 WiFi 与 ESP8266 通信（不走 USB 串口），GPIO3 可以专门用于监控。

### 3.4 状态指示 LED

NodeMCU 板载蓝色 LED（GPIO2 / D4）：

| LED 状态 | 含义 |
|---|---|
| 常灭 | 空闲 |
| 常亮 | WiFi 已连接，等待操作 |
| 常亮（烧录中） | 烧录进行中 |
| 常亮（监控中） | 监控进行中 |

> 简化实现：进入烧录/监控时 LED 常亮，退出时熄灭。WiFi 连接状态请以 WebUI 顶部"IP"显示为准。

---

## 4. 首次部署

### 4.1 安装 PlatformIO

```bash
pip install platformio
```

或安装 [VSCode PlatformIO IDE 插件](https://marketplace.visualstudio.com/items?itemName=platformio.platformio-ide)。

### 4.2 编译 ESP8266 固件

在项目根目录执行：

```bash
python -m platformio run
```

看到 `SUCCESS` 即编译成功。

### 4.3 烧录 ESP8266 固件

把 NodeMCU 用 USB 接到电脑，假设识别为 `COM5`：

```bash
python -m platformio run -t upload --upload-port COM5
```

> Linux/Mac 用户端口名形如 `/dev/ttyUSB0` 或 `/dev/cu.SLAB_USBtoUART`。

### 4.4 上传 WebUI 静态文件到 LittleFS

固件本身不包含 `index.html` 等前端文件，需要单独上传到 LittleFS 文件系统：

```bash
python -m platformio run -t uploadfs --upload-port COM5
```

> 每次更新 `data/` 目录下的 `index.html` / `style.css` / `app.js` / `config.json` 后，都要重新执行此命令。

> **重要提示**：若 NodeMCU 的 GPIO3 (RX) 已接 CC2530 的 P0_3，烧录/上传前请先**拔掉这根线**，否则 CC2530 的串口输出会干扰 ESP8266 的下载通信，导致 `Failed to connect to ESP8266: Timed out waiting for packet header` 错误。其他 3 根线（RST/DC/DD）可保留不动。

### 4.5 首次启动 + 配网

1. 上传完成后，NodeMCU 自动重启
2. 默认 WiFi 配置为空，ESP8266 进入 **配网模式**（开放 AP，无密码）
3. AP 名称：`CCLoader-Setup`
4. 电脑/手机连接此 AP（**注意：AP 是开放的，无需密码**）
5. 连上后系统会自动弹出配网页；若无弹出，浏览器访问 <http://192.168.4.1/> 或任意 URL（captive portal 会重定向到主页）
6. 切到"设置"标签页，找到"WiFi 配网"卡片
7. 点"扫描网络"，等待 3-5 秒返回可用 WiFi 列表
8. 列表中点选你的 WiFi（自动填入 SSID），输入密码
9. 点"连接"按钮
   - ESP8266 同步尝试连接（8 秒超时）
   - 成功：自动保存配置 + 切换到 STA 模式，**无需重启**
   - 失败：自动切回 AP 模式，可重试
10. 连接成功后页面会显示新 IP，按提示将电脑/手机切回原 WiFi，访问新 IP 即可

> 串口监视器可同步查看连接过程：

```bash
python -m platformio device monitor -p COM5 -b 115200
```

输出示例：

```
CCLoader WebUI booting...
Config mode (no config): AP 'CCLoader-Setup' open, IP: 192.168.4.1
CCLoader WebUI ready
HTTP: 80, SSE: 81
WiFi scan start...
WiFi scan done, found 12 networks
Trying connect to MyWiFi ...
Connected, IP: 192.168.1.100
```

11. 之后用任何连同一 WiFi 的设备访问 `http://192.168.1.100/` 即可

### 4.6 修改 WiFi 配置

若需切换 WiFi：在 WebUI "设置"页"WiFi 配网"卡片重新扫描+连接即可，**无需重启**。若忘记当前 WiFi 密码，可长按 NodeMCU 的 FLASH 按键 + 复位进入下载模式重新烧录固件，或删除 LittleFS 中的 `/config.json` 重置配置。

---

## 5. 浏览器操作流程

### 5.1 烧录 CC2530

1. 浏览器访问 ESP8266 的 IP（如 `http://192.168.1.100/`）
2. 默认在"烧录"标签页
3. 在"上传固件"区域点"选择文件"，选 BIN 或 HEX 文件
   - **BIN 文件**：直接上传（如 `DIYRuZRT_256k.bin`）
   - **HEX 文件**：浏览器自动转换为 BIN（参考 `diyruz_rt/Tools/hex2bin.py`）
     - 支持 Intel HEX 全部记录类型（00 数据/01 结束/02 段地址/04 线性地址/05 起始地址）
     - 自动校验每行 checksum，失败会提示行号
     - 自动填充 0xFF 到 256KB（0x40000），适配 CCLoader 要求
     - 转换日志会显示在上传区域：数据记录数、地址范围、BIN 大小等
4. 上传成功后，下方"已上传固件"列表会出现该文件
5. 点文件名选中（蓝色边框高亮）
6. 可选勾选"烧录后校验"
7. 点"开始烧录"
   - 进度条实时显示百分比和块进度
   - 日志区显示"开始烧录"、"总块数"、"烧录完成"
8. 烧录完成后 1 秒自动跳到"监控"标签页

> HEX 转 BIN 在浏览器端完成，不占用 ESP8266 资源。256KB 的 BIN 上传到 LittleFS 约需 5-10 秒。

### 5.2 监控 CC2530 串口

1. 切到"监控"标签页
2. 选择波特率（CC2530 默认 115200，PTVO/Z-Stack 通常 115200 或 57600）
3. 点"开始监控"
4. 状态变为"监控中 @ 115200 bps"
5. **按下 CC2530 的 RESET 按钮**（或重新上电），即可看到从 `main()` 第一行开始的完整启动日志
6. 工具栏功能：
   - **暂停**：暂停显示（数据仍接收，恢复后跳跃到最新）
   - **清空**：清空当前显示
   - **下载**：导出已显示的日志为 `.log` 文件
   - **搜索过滤**：只显示包含关键字的行
7. 接收字节数实时显示在底部
8. 点"停止"退出监控，Serial 恢复 115200，可继续烧录

### 5.3 设置

- **WiFi 配置**：修改 SSID / 密码，保存后重启生效
- **默认监控**：设置默认波特率（监控页会同步）
- **默认烧录**：设置默认是否校验（烧录页会同步）
- **设备信息**：查看 IP / RSSI / 运行时长，点"重启 ESP8266"远程重启

---

## 6. 工作原理

### 6.1 三态状态机

```
        ┌──────────────────────────────────┐
        │                                  │
        ▼                                  │
    ┌────────┐  POST /api/burn   ┌─────────┐
    │  IDLE  │ ─────────────────▶│ BURNING │
    └────────┘                   └─────────┘
        │                              │
        │ POST /api/monitor            │ 烧录完成/失败
        ▼                              │
    ┌──────────┐                       │
    │ MONITOR  │◀──────────────────────┘
    └──────────┘
        │
        │ POST /api/stop
        ▼
    ┌────────┐
    │  IDLE  │
    └────────┘
```

| 状态 | GPIO3 用途 | Serial | HTTP API |
|---|---|---|---|
| IDLE | 上位机串口（可选） | 115200 | 所有 |
| BURNING | 不使用（CC Debug 走 DD） | 115200 | 仅 /api/status |
| MONITORING | 接收 CC2530 日志 | CC2530 波特率 | /api/status + /api/stop |

### 6.2 端口分工

| 端口 | 用途 |
|---|---|
| 80  | HTTP 服务器：WebUI 页面、文件上传、API |
| 81  | SSE 服务器：实时推送烧录进度 + 监控数据（浏览器 `EventSource` 自动连接） |

### 6.3 通信协议

- **HTTP REST API**（详见源码 `initHttpRoutes()`）：
  - `GET /api/status` - 当前状态
  - `GET/POST /api/config` - 读/写配置
  - `POST /api/upload` - 上传 BIN
  - `POST /api/burn` - 开始烧录
  - `POST /api/monitor` - 开始监控
  - `POST /api/stop` - 停止监控
  - `GET /api/files` - 已上传 BIN 列表
  - `DELETE /api/files/{name}` - 删除 BIN
  - `POST /api/reboot` - 重启 ESP8266
  - `GET /api/wifi/scan` - 扫描周围 WiFi，返回 SSID/RSSI/加密类型
  - `POST /api/wifi/connect` - 连接指定 WiFi（body: `{ssid, password}`），成功后切换 STA 模式

- **SSE 事件**（端口 81，`event: message`，`data` 为 JSON）：
  - `{"type":"status","state":"idle|burning|monitoring"}`
  - `{"type":"burn_progress","percent":50,"current_block":100,"total_blocks":200,"done":false,"error":""}`
  - `{"type":"monitor_start","baud":115200}`
  - `{"type":"monitor_data","data":"<base64>"}` - 监控数据 Base64 编码
  - `{"type":"monitor_stop"}`
  - `{"type":"wifi_connected","ssid":"...","ip":"192.168.x.x"}` - WiFi 连接成功
  - `{"type":"wifi_connect_failed","ssid":"..."}` - WiFi 连接失败

### 6.4 无外部库依赖

为避免 PlatformIO 在线安装库的网络限制，本固件完全使用 ESP8266 Arduino Core 自带功能：

| 原方案 | 改造后 |
|---|---|
| WebSocketsServer (Markus Sattler) | 原生 `WiFiServer` (端口 81) 实现 SSE |
| ArduinoJson 6.x | `String` 手工拼接 + 简易 JSON 解析（仅顶层简单字段） |

---

## 7. 故障排查

### 7.1 编译相关

| 问题 | 原因 | 解决 |
|---|---|---|
| `UnknownPackageError: Could not find the package with 'WebSockets'` | 旧版本 `lib_deps` 仍引用外部库 | 已移除，重新拉取最新代码或确认 `platformio.ini` 中无 `lib_deps` |
| `'UriRegex' was not declared` | 未包含头文件 | 已在源码顶部 `#include <uri/UriRegex.h>` |
| `LittleFS mount failed!` | 未执行 `uploadfs` | 见 4.4 节 |
| `platforms.lock` 沙箱阻止 | PlatformIO 试图写用户目录 | 用管理员权限运行，或检查杀软白名单 |
| `Failed to connect to ESP8266: Timed out waiting for packet header` | GPIO3 (RX) 接到 CC2530 P0_3，CC2530 输出干扰 ESP8266 下载 | **拔掉 GPIO3↔P0_3 的连接线后重试**；或按住 FLASH + RESET 进入强制下载模式 |

### 7.2 运行时

| 现象 | 排查 |
|---|---|
| 浏览器打不开 IP | 1. 确认电脑/手机与 ESP8266 在同一 WiFi<br>2. 串口监视器看启动日志中的 IP<br>3. 防火墙放行 80/81 端口（仅本机测试时） |
| 启动后是 AP 模式（`CCLoader-Setup`）而不是连 WiFi | 1. 之前未配网；2. 配置的 WiFi 连不上（密码错/信号弱）；进入"设置"页重新配网 |
| 配网连接失败 | 1. 密码错误（最常见）<br>2. WiFi 信号弱（RSSI < -85dBm）<br>3. SSID 含中文或特殊字符（建议 ASCII）<br>4. ESP8266 仅支持 2.4GHz，5GHz 无法连接 |
| 手机连 AP 后未自动弹出配网页 | 1. 访问任意 URL（如 `http://1.2.3.4/`）触发 captive portal<br>2. 或直接访问 <http://192.168.4.1/> |
| SSE 一直"已断开" | 1. 端口 81 被防火墙拦截<br>2. 浏览器扩展拦截 EventSource<br>3. ESP8266 重启中，等 2 秒会自动重连 |
| 烧录卡 0% 不动 | 1. CC2530 没接好（检查 4 根线）<br>2. CC2530 已锁死（接错电源）<br>3. 看 SSE 日志区的错误信息 |
| 烧录报 `chip not detected` | 1. DD/DC/RESET 三根线任一接错<br>2. CC2530 模块没供电<br>3. 共地未连接 |
| 烧录报 `XOSC timeout` | CC2530 外部晶振未起振 | 模块硬件问题，更换 |
| 烧录报 `verify failed at block N` | 1. DD 线接触不良<br>2. Flash 寿命耗尽 | 重新插紧；或换 CC2530 |
| HEX 转换失败 | 1. 文件不是标准 Intel HEX 格式<br>2. 检查行号提示，定位出错行<br>3. 用 `python hex2bin.py xx.hex` 验证 |
| 监控无数据 | 1. P0_3 → RX 接错<br>2. 波特率不对（PTVO 默认 115200，部分 Z-Stack 用 57600）<br>3. CC2530 固件未配置 UART0 输出 |
| 监控有乱码 | 波特率不匹配 | 改波特率重试，常见：9600/57600/115200/230400 |
| 监控卡顿、丢数据 | WiFi 信号差 | RSSI 应 > -75dBm；或缩短距离 |
| 上传 BIN 失败 | LittleFS 空间不足 | 删除旧 BIN；或换 4MB Flash 的 NodeMCU |

### 7.3 串口调试

用 PlatformIO 串口监视器查看 ESP8266 日志：

```bash
python -m platformio device monitor -p COM5 -b 115200
```

> 监控模式下 Serial 会切换到 CC2530 波特率，此时 USB 串口监视器会看到 CC2530 的乱码（因波特率不同）。退出监控后自动恢复 115200。

---

## 8. 常见问题

### Q1: 可以同时连接多个浏览器吗？

可以。SSE 最多支持 4 个并发客户端（`SSE_MAX_CLIENTS = 4`）。所有客户端都会收到相同的进度/监控事件。但烧录/监控 API 同一时间只能执行一个。

### Q2: 烧录时能同时监控吗？

不能。状态机保证互斥（见 6.1 节）。烧录完成后会自动切到监控页。

### Q3: CC2530 已经焊在板子上，能直接烧吗？

只要 CC2530 的 `RESETn`、`DC`、`DD`、`P0_3`、`VCC`、`GND` 引出即可，与是否焊接无关。

### Q4: 旧的 CCLoader.exe 还能用吗？

不能。本固件已完全重写为 WebUI 方案，不再支持 `CCLoader.exe` 的命令行协议。如需保留旧方式，请用 git 切换到改造前的版本。

### Q5: 能在 ESP32 上用吗？

不能直接用。源码使用 ESP8266 特有的 `ESP8266WebServer`、`LittleFS`、`WiFiServer` API，ESP32 需要相应改造（库不同）。

### Q6: 如何修改 WebUI 界面？

编辑 `data/` 目录下的 `index.html` / `style.css` / `app.js`，然后重新执行：

```bash
python -m platformio run -t uploadfs --upload-port COM5
```

无需重新烧录固件。

### Q7: 监控数据为什么用 Base64 编码？

CC2530 的串口日志可能包含二进制字节（如 `\r\n`、控制字符、非 ASCII），直接放 JSON 会破坏格式。Base64 保证传输安全，浏览器端 `atob()` + `TextDecoder('utf-8')` 还原。

### Q8: 监控模式下还能访问 WebUI 吗？

能。监控只占用 GPIO3（Serial RX），HTTP/WebUI 走 WiFi，可正常访问。但 Serial 串口监视器会显示乱码（波特率已切换）。

---

## 附：项目结构

```
D:\VC\CCLoader\
├── src\
│   └── CCLoader.ino          # ESP8266 固件主文件（含 CC Debug 协议 + WebUI）
├── data\                      # LittleFS 上传目录
│   ├── index.html             # WebUI 主页（三标签 SPA）
│   ├── style.css              # 暗色主题响应式样式
│   ├── app.js                 # 前端逻辑（SSE + fetch API）
│   └── config.json            # 默认配置
├── platformio.ini             # PlatformIO 编译配置（无 lib_deps）
├── Windows\                   # 旧版 CCLoader.exe（仅保留，本固件不再使用）
│   └── DIYRuZRT_256k.bin      # 可用于测试烧录的 CC2530 固件
├── Bin\                       # 其他 CC2541 BIN（参考）
├── Arduino\CCLoader\          # 原始 CCLib Arduino 代码（保留作参考）
├── cc_monitor.py              # 旧版 Python 串口监控脚本（已被 WebUI 取代）
└── README.md                  # 本文档
```
