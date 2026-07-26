# CCLoader API 参考

> 本文件描述 CCLoader 烧录器的 HTTP REST API 和 SSE 事件协议。

**固件版本**：CCLoader-WebUI v1.1
**适用设备**：NodeMCU ESP8266 + CC2530 烧录/监控一体机

---

## 目录

1. [概述](#1-概述)
2. [三态状态机](#2-三态状态机)
3. [端口分工](#3-端口分工)
4. [HTTP API 完整列表](#4-http-api-完整列表)
5. [端点详细说明](#5-端点详细说明)
6. [SSE 事件类型完整列表](#6-sse-事件类型完整列表)
7. [状态机互斥规则](#7-状态机互斥规则)
8. [异步烧录流程示例](#8-异步烧录流程示例)
9. [监控日志获取两种方式](#9-监控日志获取两种方式)
10. [关键限制](#10-关键限制)

---

## 1. 概述

CCLoader 固件对外提供两类网络接口：

| 协议 | 端口 | 用途 |
|---|---|---|
| HTTP REST API | **80** | WebUI 页面、文件上传、配置管理、烧录/监控控制、OTA 升级 |
| SSE（Server-Sent Events） | **81** | 实时推送烧录进度、监控数据、状态变更、WiFi 事件 |

### 特性

- **无鉴权**：局域网内任意设备均可调用，部署时请确保网络可信
- **无外部库依赖**：HTTP 使用 `ESP8266WebServer`，SSE 使用原生 `WiFiServer`，JSON 使用 `String` 手工拼接/解析
- **强制异步烧录**：`/api/burn` 立即返回 `task_id`，烧录在后台执行，避免 HTTP 长连接超时
- **强制校验**：烧录后自动回读验证，保证烧录正确性，客户端无法跳过
- **三态互斥**：状态机保证同一时间只执行烧录或监控之一
- **NTP 授时**：WiFi 连接后自动同步北京时间（UTC+8），未授时时 `time` 字段为 0

### 基地址

```
http://<ESP8266-IP>/
```

- STA 模式：路由器分配的 IP（如 `192.168.1.100`）
- AP 配网模式：`192.168.4.1`（开放 AP，SSID 为 `CCLoader-Setup`）

---

## 2. 三态状态机

CCLoader 通过三态状态机保证烧录与监控的互斥性，避免 GPIO3（RX）引脚冲突和 CC2530 串口通信冲突。

### 2.1 状态定义

| 状态 | 名称 | GPIO3 用途 | Serial 波特率 | 说明 |
|---|---|---|---|---|
| `IDLE` | 空闲 | 上位机串口（可选） | 115200 | 默认状态，所有 API 可用 |
| `BURNING` | 烧录中 | 不使用（CC Debug 走 DD/DC） | 115200 | 后台烧录 BIN 到 CC2530，进度通过 SSE 推送 |
| `MONITORING` | 监控中 | 接收 CC2530 串口日志 | CC2530 波特率 | Serial 切换到目标波特率，接收 P0_3 日志 |

### 2.2 状态转换图

```
        ┌────────────────────────────────────────┐
        │                                        │
        ▼                                        │
    ┌────────┐   POST /api/burn      ┌─────────┐  │
    │        │ ─────────────────────▶│         │  │
    │  IDLE  │                       │ BURNING │  │
    │        │◀─────────────────────│         │  │
    └────────┘   烧录完成/失败         └─────────┘  │
        │                                        │
        │ POST /api/nvreset                       │ 烧录完成/失败
        │ POST /api/backup                        │
        │ POST /api/monitor                        │
        ▼                                        │
    ┌────────────┐                                │
    │            │◀───────────────────────────────┘
    │ MONITORING │
    │            │
    └────────────┘
        │
        │ POST /api/stop
        ▼
    ┌────────┐
    │  IDLE  │
    └────────┘
```

### 2.3 状态转换表

| 当前状态 | 触发动作 | 目标状态 | 说明 |
|---|---|---|---|
| `IDLE` | `POST /api/burn` | `BURNING` | 设置 `burn_pending=true`，立即返回 `task_id`，loop() 中执行烧录 |
| `IDLE` | `POST /api/nvreset` | `BURNING` | 设置 `nvreset_pending=true`，loop() 中执行清除配网 |
| `IDLE` | `POST /api/backup` | `BURNING` | 设置 `backup_pending=true`，loop() 中执行备份 |
| `IDLE` | `POST /api/monitor` | `MONITORING` | 切换 Serial 波特率，可选自动复位 CC2530 |
| `BURNING` | 烧录完成/失败 | `IDLE` | 推送最终 `burn_progress` 事件，清空 `burn_pending` |
| `MONITORING` | `POST /api/stop` | `IDLE` | 恢复 Serial 到 115200，推送 `monitor_stop` 事件 |

> **注**：`nvreset` 和 `backup` 复用 `BURNING` 状态以利用互斥逻辑，进度通过 `burn_progress` 事件推送。

---

## 3. 端口分工

| 端口 | 协议 | 服务 | 说明 |
|---|---|---|---|
| **80** | HTTP | `ESP8266WebServer` | WebUI 页面、REST API、OTA 升级（`/update`） |
| **81** | SSE | 原生 `WiFiServer` | 实时事件推送，浏览器 `EventSource` 或 `curl -N` 连接 |

### SSE 连接说明

- 最大并发客户端：**4** 个（第 5 个连接会被拒绝并关闭）
- 响应头：`Content-Type: text/event-stream`、`Cache-Control: no-cache`、`Connection: keep-alive`、`Access-Control-Allow-Origin: *`
- 事件格式：`event: message\ndata: <JSON>\n\n`
- 连接建立后立即推送一次 `status` 事件（当前状态）

### SSE 连接示例

```bash
# curl 监听 SSE 事件流
curl -N http://192.168.1.100:81/
```

```javascript
// 浏览器 EventSource
const es = new EventSource('http://192.168.1.100:81/');
es.onmessage = (e) => {
  const msg = JSON.parse(e.data);
  console.log(msg.type, msg);
};
```

---

## 4. HTTP API 完整列表

### 4.1 REST API 端点

| 方法 | 路径 | 功能 | 返回码 |
|---|---|---|---|
| GET | `/api/status` | 当前状态（state/burn/monitor/wifi/task_id/time） | 200 |
| GET | `/api/config` | 读取配置 | 200 |
| POST | `/api/config` | 更新配置（部分字段合并保存） | 200 |
| POST | `/api/upload` | 上传 BIN（multipart/form-data，字段 `file`） | 200 / 400 / 500 |
| GET | `/api/files` | 列出已上传 BIN（含 size/time） | 200 |
| DELETE | `/api/files/{name}` | 删除指定 BIN | 200 / 400 / 403 / 404 |
| POST | `/api/burn` | 异步烧录（强制校验，立即返回 task_id） | 202 / 404 / 409 |
| POST | `/api/nvreset` | 清除配网（保留固件，异步） | 202 / 409 |
| POST | `/api/backup` | 备份固件到 LittleFS（异步） | 202 / 409 |
| POST | `/api/monitor` | 开始监控（body: `{baud, auto_reset}`） | 200 / 400 / 409 |
| POST | `/api/stop` | 停止监控 | 200 |
| POST | `/api/reset` | 复位 CC2530（GPIO5/RESETn，监控中也可用） | 200 / 409 |
| GET | `/api/monitor/buffer` | 获取监控日志（可选 `?since=N&max_bytes=M`） | 200 |
| GET | `/api/help` | 返回帮助文档（text/plain，markdown 格式） | 200 |
| GET | `/api/wifi/scan` | 扫描周围 WiFi（耗时 3-5 秒） | 200 |
| POST | `/api/wifi/connect` | 连接 WiFi（body: `{ssid, password}`） | 200 / 400 |
| POST | `/api/reboot` | 重启 ESP8266 | 200 |

### 4.2 WebUI 与 OTA 端点

| 方法 | 路径 | 功能 | 返回码 |
|---|---|---|---|
| GET | `/` | WebUI 主页（index.html，PROGMEM 内嵌） | 200 |
| GET | `/style.css` | 样式表（PROGMEM 内嵌） | 200 |
| GET | `/app.js` | 前端脚本（PROGMEM 内嵌） | 200 |
| POST | `/update` | OTA 固件升级（multipart，字段 `image`） | 200 |

> **注**：`?async=1` 参数对 `/api/burn` 仍兼容但非必需（已强制异步）。

---

## 5. 端点详细说明

### 5.1 GET /api/status

获取设备当前完整状态。Agent 轮询跟踪烧录进度的首选接口。

**响应示例**：

```json
{
  "state": "burning",
  "config_mode": false,
  "burn": {
    "percent": 45,
    "current_block": 230,
    "total_blocks": 512,
    "done": false,
    "error": ""
  },
  "monitor": {
    "active": false,
    "baud": 115200,
    "bytes_received": 0
  },
  "wifi": {
    "ssid": "MyWiFi",
    "ip": "192.168.1.100",
    "rssi": -55,
    "mode": "sta"
  },
  "uptime": 3600,
  "task_id": 1,
  "burn_pending": false,
  "time": 1729000000
}
```

**字段说明**：

| 字段 | 类型 | 说明 |
|---|---|---|
| `state` | string | `idle` / `burning` / `monitoring` |
| `config_mode` | bool | true 表示处于 AP 配网模式 |
| `burn.percent` | int | 烧录进度百分比（0-100） |
| `burn.current_block` | int | 当前已烧录块数 |
| `burn.total_blocks` | int | 总块数（256KB BIN = 512 块） |
| `burn.done` | bool | 烧录是否完成（含失败） |
| `burn.error` | string | 错误信息（空表示无错误） |
| `monitor.active` | bool | 是否处于监控状态 |
| `monitor.baud` | int | 当前监控波特率 |
| `monitor.bytes_received` | int | 累计接收字节数 |
| `wifi.ssid` | string | 当前连接的 SSID |
| `wifi.ip` | string | 设备 IP（STA 或 AP） |
| `wifi.rssi` | int | WiFi 信号强度（dBm，AP 模式为 0） |
| `wifi.mode` | string | `sta` / `ap` / `none` |
| `uptime` | int | 启动后运行秒数 |
| `task_id` | int | 当前/最近一次烧录任务 ID（单调递增） |
| `burn_pending` | bool | 是否有待执行的烧录任务 |
| `time` | int | 当前 epoch 秒（北京时间 CST-8，未授时为 0） |

---

### 5.2 GET /api/config

读取当前配置。

**响应示例**：

```json
{
  "wifi_ssid": "MyWiFi",
  "wifi_password": "mypassword",
  "monitor_baud": 115200,
  "verify": false
}
```

> **注**：`verify` 字段为默认配置，`/api/burn` 内部强制 verify=true，忽略此字段。

---

### 5.3 POST /api/config

更新配置。**部分更新**：仅更新请求中出现的字段，未出现的字段保持原值。

**请求体**（任意子集）：

```json
{
  "wifi_ssid": "NewWiFi",
  "wifi_password": "newpassword",
  "monitor_baud": 57600,
  "verify": true
}
```

**响应**：

```json
{"success": true}
```

> **注**：修改 `wifi_ssid` / `wifi_password` 仅保存到 LittleFS，需调用 `/api/wifi/connect` 或 `/api/reboot` 生效。

---

### 5.4 POST /api/upload

上传 BIN 文件到 LittleFS。**仅接受 `.bin`**，`.hex` 会被拒绝。

**请求**：

- Content-Type: `multipart/form-data`
- 字段名：`file`
- 文件名会自动去除路径分隔符（`/` 和 `\`）
- 同名旧文件会被自动删除覆盖

**.bin 成功响应**（200）：

```json
{
  "success": true,
  "filename": "CC2530.bin",
  "size": 262144
}
```

**.hex 拒绝响应**（400）：

```json
{
  "error": "hex_not_supported",
  "message": "API 不支持 .hex 直传，请先在客户端转换为 .bin 再上传",
  "hint": "浏览器端上传 .hex 会自动 hex2bin；API 调用需自行转换..."
}
```

**写入失败响应**（500）：

```json
{"error": "write failed"}
```

**curl 示例**：

```bash
curl -F "file=@CC2530.bin" http://192.168.1.100/api/upload
```

> **注**：`.hex` 转换算法详见 `/api/help` 返回的帮助文档第 2.3 节，或参考 `data/app.js` 中的 `hex2bin()` 实现。

---

### 5.5 GET /api/files

列出 LittleFS 中已上传的 BIN 文件（自动过滤 `config.json` 和 WebUI 静态文件）。

**响应示例**：

```json
{
  "success": true,
  "files": [
    {
      "name": "CC2530.bin",
      "size": 262144,
      "time": 1729000000
    },
    {
      "name": "backup_20250115_143022.bin",
      "size": 262144,
      "time": 1729000200
    }
  ]
}
```

| 字段 | 说明 |
|---|---|
| `name` | 文件名 |
| `size` | 文件大小（字节） |
| `time` | 文件时间戳（epoch 秒，NTP 未授时为 0，显示为 1970 年） |

---

### 5.6 DELETE /api/files/{name}

删除指定 BIN 文件。

**路径参数**：`{name}` - 文件名（URL 编码，自动解码）

**安全检查**：
- 禁止删除 `config.json` / `index.html` / `style.css` / `app.js`
- 禁止路径穿越（文件名含 `/` 或 `\`）

**响应**：
- 成功（200）：`{"success": true}`
- 文件名缺失（400）：`{"error": "no filename"}`
- 禁止删除（403）：`{"error": "forbidden"}`
- 文件不存在（404）：`{"error": "not found"}`

**curl 示例**：

```bash
curl -X DELETE http://192.168.1.100/api/files/CC2530.bin
```

---

### 5.7 POST /api/burn

发起异步烧录。**强制异步 + 强制校验**，立即返回 `task_id`，烧录在 `loop()` 中后台执行。

**请求体**：

```json
{
  "filename": "CC2530.bin",
  "verify": true
}
```

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `filename` | string | 否（默认 `firmware.bin`） | LittleFS 中的 BIN 文件名 |
| `verify` | bool | 否 | **被忽略，强制 true**（响应中 `verify_forced: true`） |

**成功响应**（202 Accepted）：

```json
{
  "success": true,
  "async": true,
  "task_id": 1,
  "total_blocks": 512,
  "verify": true,
  "verify_forced": true
}
```

**错误响应**：
- 状态非 IDLE 或已有 pending 任务（409）：`{"error": "busy"}`
- 文件不存在（404）：`{"error": "file not found"}`

**烧录进度跟踪**：通过轮询 `/api/status` 的 `burn` 字段或监听 SSE `burn_progress` 事件。

**烧录错误类型**（`burn.error` 字段）：
- `chip not detected` - CC2530 未连接或 DD/DC/RESET 线接错
- `XOSC timeout` - 外部晶振未起振（硬件问题）
- `verify failed at block N` - 第 N 块校验失败（DD 线接触不良或 Flash 寿命耗尽）
- `file not found: xxx` - 文件不存在

---

### 5.8 POST /api/nvreset

清除 CC2530 的 Zigbee 配网信息，**保留固件**。

**流程**：读取 CC2530 全部 Flash → 清除尾部 NV 区域（最后 4KB 填 0xFF）→ 全片擦除 → 写回。耗时约 2 分钟。

**响应**（202）：

```json
{
  "success": true,
  "async": true,
  "info": "清除配网：读取 Flash → 清除 NV → 写回"
}
```

**错误响应**：
- 状态非 IDLE 或已有 pending 任务（409）：`{"error": "busy"}`

**进度跟踪**：复用 `burn_progress` 事件，进度百分比含义：
- 0-50%：读取 Flash 阶段
- 50-100%：擦除+写回阶段

---

### 5.9 POST /api/backup

备份 CC2530 全部 Flash（256KB）到 LittleFS，生成 `backup_YYYYMMDD_HHMMSS.bin` 文件。

**响应**（202）：

```json
{
  "success": true,
  "async": true,
  "info": "备份固件：读取 Flash 保存到 LittleFS"
}
```

**错误响应**：
- 状态非 IDLE 或已有 pending 任务（409）：`{"error": "busy"}`

**进度跟踪**：复用 `burn_progress` 事件，完成后 `burn.info` 字段包含生成的文件名。

**文件名时间戳**：依赖 NTP 授时。未授时时使用毫秒时间戳命名（如 `backup_1234567.bin`）。

> 备份完成后可通过 `/api/files` 列表查看，通过浏览器下载或 `/api/files/{name}` 删除。

---

### 5.10 POST /api/monitor

进入监控模式，Serial 切换到 CC2530 波特率，开始接收 P0_3 串口日志。

**请求体**：

```json
{
  "baud": 115200,
  "auto_reset": true
}
```

| 字段 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| `baud` | int | 否 | 115200 | CC2530 串口波特率，范围 9600-230400 |
| `auto_reset` | bool | 否 | false | true 时进入监控前自动复位 CC2530（捕获启动日志） |

**成功响应**（200）：

```json
{
  "success": true,
  "baud": 115200
}
```

**错误响应**：
- 状态非 IDLE（409）：`{"error": "busy"}`
- baud 无效（400）：`{"error": "invalid baud"}`

**SSE 事件**：进入监控后立即推送 `monitor_start` 事件。

> **常见波特率**：CC2530 默认 115200，PTVO 默认 115200，部分 Z-Stack 用 57600，调试输出可能用 9600 或 230400。

---

### 5.11 POST /api/stop

停止监控，恢复 Serial 到 115200，返回 IDLE 状态。

**响应**（200）：

```json
{"success": true}
```

**行为**：
- 监控中：推送残留数据，恢复 Serial，推送 `monitor_stop` 事件
- 非监控中：仅返回 200，不执行任何操作

---

### 5.12 POST /api/reset

通过 GPIO5/RESETn 复位 CC2530，**无需手动按 RESET 按钮**。

**响应**（200）：

```json
{"success": true}
```

**错误响应**：
- 烧录中（409）：`{"error": "busy"}`

**监控中复位的特殊行为**：
- 先推送残留监控数据
- 执行复位（拉低 RESETn 10ms）
- 推送 `monitor_reset` 事件，前端清空日志区准备接收启动日志
- **不影响后端环形缓冲计数**：`g_monitor_ring_total` 保持单调递增，Agent 断点续传语义不变

---

### 5.13 GET /api/monitor/buffer

获取监控日志环形缓冲。**支持断点续传**，适合 Agent 轮询获取完整日志。

**查询参数**：

| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `since` | int | 0 | 返回从累计字节 N 之后的数据（断点续传偏移） |
| `max_bytes` | int | 4096 | 限制单次返回最大字节数（上限 8192） |

**响应**（流式分块输出，避免堆 OOM）：

```json
{
  "success": true,
  "total": 16384,
  "offset": 12288,
  "bytes": 4096,
  "truncated": false,
  "data": "<base64>"
}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `total` | int | 累计接收字节数（单调递增，不取模） |
| `offset` | int | 本次返回数据的起始累计偏移 |
| `bytes` | int | 本次返回的原始字节数 |
| `truncated` | bool | true 表示请求的 `since` 已超出缓冲范围，部分数据被覆盖 |
| `missed` | int | 仅 `truncated=true` 时存在，丢失的字节数 |
| `data` | string | Base64 编码的日志数据 |

**特殊情况**：
- `since >= total`：返回空 `data`，`bytes: 0`（无新数据）
- `since < oldest`（数据已被覆盖）：`truncated: true`，`missed` 字段表示丢失字节数，从 `oldest` 开始返回

**curl 示例**：

```bash
# 获取最近 4096 字节
curl http://192.168.1.100/api/monitor/buffer

# 断点续传，从字节 8192 之后获取
curl "http://192.168.1.100/api/monitor/buffer?since=8192"

# 限制单次返回 2048 字节
curl "http://192.168.1.100/api/monitor/buffer?since=0&max_bytes=2048"
```

> **注**：环形缓冲大小 8192 字节，超出后旧数据被覆盖。Agent 应在 `total` 差值超过 8192 前及时拉取。

---

### 5.14 GET /api/help

返回嵌入式帮助文档（markdown 纯文本，AI Agent 和 WebUI 帮助页共用）。

**响应**：
- Content-Type: `text/plain; charset=utf-8`
- Body: `help.md` 文件内容（PROGMEM 内嵌）

---

### 5.15 GET /api/wifi/scan

扫描周围可用 WiFi 网络，耗时 3-5 秒。

**响应示例**：

```json
{
  "success": true,
  "networks": [
    {
      "ssid": "MyWiFi",
      "rssi": -55,
      "encrypted": true,
      "enc_type": 4
    },
    {
      "ssid": "OpenWiFi",
      "rssi": -72,
      "encrypted": false,
      "enc_type": 7
    }
  ]
}
```

| 字段 | 说明 |
|---|---|
| `ssid` | 网络 SSID |
| `rssi` | 信号强度（dBm） |
| `encrypted` | 是否加密 |
| `enc_type` | 加密类型（ESP8266 `encryptionType()` 枚举值） |

> ESP8266 仅支持 2.4GHz，5GHz 网络不会出现在扫描结果中。

---

### 5.16 POST /api/wifi/connect

连接指定 WiFi，成功后保存配置并切换到 STA 模式，**无需重启**。

**请求体**：

```json
{
  "ssid": "MyWiFi",
  "password": "mypassword"
}
```

**响应**（立即返回，连接过程异步执行）：

```json
{
  "success": true,
  "message": "connecting"
}
```

**错误响应**（400）：
- SSID 为空：`{"error": "ssid required"}`
- SSID 过长（>32 字节）：`{"error": "ssid too long"}`

**连接结果**：通过 SSE 推送：
- 成功：`{"type": "wifi_connected", "ssid": "...", "ip": "192.168.x.x"}`
- 失败：`{"type": "wifi_connect_failed", "ssid": "..."}`（自动回退 AP 模式）

**连接超时**：8 秒。成功后自动保存配置 + 启动 NTP 授时。

---

### 5.17 POST /api/reboot

重启 ESP8266。

**响应**（200）：

```json
{"success": true}
```

> 响应后延时 200ms 执行 `ESP.restart()`，确保响应已发送。

---

### 5.18 POST /update（OTA 升级）

OTA 远程升级 ESP8266 固件，由 `ESP8266HTTPUpdateServer` 提供。

**请求**：
- Content-Type: `multipart/form-data`
- 字段名：`image`
- 文件：编译生成的 `.bin` 固件（`.pio/build/nodemcuv2/firmware.bin`）

**特性**：
- LittleFS 保留（`/config.json`、已上传 BIN 文件不丢失）
- WiFi 配置保留
- 升级后自动重启
- 升级期间（约 10-30 秒）HTTP/SSE 暂不可用

**curl 示例**：

```bash
curl -F "image=@.pio/build/nodemcuv2/firmware.bin" http://192.168.1.100/update
```

**响应**：

```
Update Success: <size> bytes
Rebooting...
```

---

## 6. SSE 事件类型完整列表

所有事件均为 `event: message`，`data` 为 JSON 字符串。`type` 字段取值如下：

### 6.1 status（状态推送）

连接建立时立即推送一次，状态变更时再次推送。

```json
{
  "type": "status",
  "state": "idle"
}
```

`state` 取值：`idle` / `burning` / `monitoring`

---

### 6.2 burn_progress（烧录进度）

烧录过程中每 16 块推送一次（约每 3% 进度），完成/失败时推送最终状态。

```json
{
  "type": "burn_progress",
  "percent": 50,
  "current_block": 256,
  "total_blocks": 512,
  "done": false,
  "error": "",
  "info": ""
}
```

| 字段 | 说明 |
|---|---|
| `percent` | 进度百分比（0-100） |
| `current_block` | 当前块数 |
| `total_blocks` | 总块数 |
| `done` | 是否完成（true 时检查 `error` 判断成功/失败） |
| `error` | 错误信息（空表示成功） |
| `info` | 附加信息（如备份完成时的文件名） |

> `nvreset` 和 `backup` 操作也复用此事件类型推送进度。

---

### 6.3 monitor_start（监控开始）

```json
{
  "type": "monitor_start",
  "baud": 115200
}
```

---

### 6.4 monitor_data（监控数据）

CC2530 串口日志，**Base64 编码**（保证二进制字节传输安全）。攒够 256 字节或 200ms 静默时推送。

```json
{
  "type": "monitor_data",
  "data": "PHJlYWw+..."
}
```

**Base64 解码**：
- 浏览器：`atob(msg.data)` + `TextDecoder('utf-8')`
- Python：`base64.b64decode(msg['data']).decode('utf-8', errors='replace')`

---

### 6.5 monitor_reset（CC2530 已复位）

监控中调用 `/api/reset` 后推送，前端清空日志区准备接收新的启动日志。

```json
{
  "type": "monitor_reset"
}
```

> 后端环形缓冲计数不重置，Agent 断点续传语义保持单调递增。

---

### 6.6 monitor_stop（监控停止）

```json
{
  "type": "monitor_stop"
}
```

---

### 6.7 wifi_connected（WiFi 连接成功）

```json
{
  "type": "wifi_connected",
  "ssid": "MyWiFi",
  "ip": "192.168.1.100"
}
```

---

### 6.8 wifi_connect_failed（WiFi 连接失败）

```json
{
  "type": "wifi_connect_failed",
  "ssid": "MyWiFi"
}
```

> 连接失败后自动回退 AP 配网模式。

---

### 6.9 事件类型汇总表

| `type` 取值 | 触发时机 | 关键字段 |
|---|---|---|
| `status` | SSE 连接建立、状态变更 | `state` |
| `burn_progress` | 烧录/备份/nvreset 进度更新 | `percent`, `done`, `error` |
| `monitor_start` | 进入监控模式 | `baud` |
| `monitor_data` | 监控收到串口数据 | `data`（Base64） |
| `monitor_reset` | 监控中复位 CC2530 | 无 |
| `monitor_stop` | 退出监控模式 | 无 |
| `wifi_connected` | WiFi 连接成功 | `ssid`, `ip` |
| `wifi_connect_failed` | WiFi 连接失败 | `ssid` |

---

## 7. 状态机互斥规则

状态机保证烧录与监控的互斥性。下表列出每个 API 在各状态下的可用性。

### 7.1 互斥规则表

| API | IDLE | BURNING | MONITORING |
|---|:---:|:---:|:---:|
| `GET /api/status` | ✅ 200 | ✅ 200 | ✅ 200 |
| `GET /api/config` | ✅ 200 | ✅ 200 | ✅ 200 |
| `POST /api/config` | ✅ 200 | ✅ 200 | ✅ 200 |
| `POST /api/upload` | ✅ 200 | ✅ 200 | ✅ 200 |
| `GET /api/files` | ✅ 200 | ✅ 200 | ✅ 200 |
| `DELETE /api/files/{name}` | ✅ 200 | ✅ 200 | ✅ 200 |
| `POST /api/burn` | ✅ 202 | ❌ 409 | ❌ 409 |
| `POST /api/nvreset` | ✅ 202 | ❌ 409 | ❌ 409 |
| `POST /api/backup` | ✅ 202 | ❌ 409 | ❌ 409 |
| `POST /api/monitor` | ✅ 200 | ❌ 409 | ❌ 409 |
| `POST /api/stop` | ✅ 200（无动作） | ✅ 200（无动作） | ✅ 200（停止监控） |
| `POST /api/reset` | ✅ 200 | ❌ 409 | ✅ 200（推送 monitor_reset） |
| `GET /api/monitor/buffer` | ✅ 200（空数据） | ✅ 200（空数据） | ✅ 200（含数据） |
| `GET /api/help` | ✅ 200 | ✅ 200 | ✅ 200 |
| `GET /api/wifi/scan` | ✅ 200 | ✅ 200 | ✅ 200 |
| `POST /api/wifi/connect` | ✅ 200 | ✅ 200 | ✅ 200 |
| `POST /api/reboot` | ✅ 200 | ✅ 200 | ✅ 200 |
| `POST /update` | ✅ 200 | ✅ 200 | ✅ 200 |

### 7.2 409 错误响应格式

所有因状态冲突被拒绝的请求返回统一格式：

```json
{"error": "busy"}
```

HTTP 状态码：**409 Conflict**

### 7.3 互斥逻辑说明

- **`burn_pending` 标志**：`POST /api/burn` 设置后，在 `loop()` 执行前，再次调用 `/api/burn` / `/api/nvreset` / `/api/backup` 会返回 409
- **`nvreset_pending` / `backup_pending` 标志**：同理，防止多个长耗时操作排队
- **`/api/reset` 在监控中可用**：复位后 CC2530 重新启动，可捕获 `main()` 启动日志，不影响后端缓冲计数
- **`/api/stop` 任何时候可用**：非监控状态下调用仅返回 200，不执行操作（可用于状态同步）

---

## 8. 异步烧录流程示例

### 8.1 完整 curl 流程

```bash
#!/bin/bash
IP=192.168.1.100
BIN=CC2530.bin

# 1. 检查设备就绪
STATUS=$(curl -s http://${IP}/api/status)
echo "当前状态: ${STATUS}"
# 应返回 "state":"idle"，否则等待或处理 busy

# 2. 上传 BIN（multipart/form-data，字段 file。仅 .bin）
curl -s -F "file=@${BIN}" http://${IP}/api/upload
# 返回: {"success":true,"filename":"CC2530.bin","size":262144}

# 3. 发起异步烧录（立即返回 task_id，强制校验）
curl -s -X POST "http://${IP}/api/burn" \
  -H "Content-Type: application/json" \
  -d '{"filename":"CC2530.bin","verify":true}'
# 返回: {"success":true,"async":true,"task_id":1,"total_blocks":512,
#        "verify":true,"verify_forced":true}

# 4. 轮询状态（每 2 秒）
while true; do
  STATUS=$(curl -s http://${IP}/api/status)
  echo "状态: ${STATUS}"
  # 解析 burn.done 字段判断完成
  echo "${STATUS}" | grep -q '"done":true' && break
  sleep 2
done
# 完成示例: {"state":"idle","task_id":1,
#           "burn":{"percent":100,"current_block":512,"total_blocks":512,
#                   "done":true,"error":""}, ...}

# 5. 检查烧录结果
if echo "${STATUS}" | grep -q '"error":""'; then
  echo "烧录成功"
else
  echo "烧录失败"
  exit 1
fi

# 6. 开始监控（自动复位 CC2530，捕获启动日志）
curl -s -X POST http://${IP}/api/monitor \
  -H "Content-Type: application/json" \
  -d '{"baud":115200,"auto_reset":true}'

# 7. 轮询获取日志（断点续传）
OFFSET=0
DEADLINE=$(($(date +%s) + 10))
while [ $(date +%s) -lt ${DEADLINE} ]; do
  RESP=$(curl -s "http://${IP}/api/monitor/buffer?since=${OFFSET}")
  echo "日志: ${RESP}"
  # 更新偏移到最新 total
  OFFSET=$(echo "${RESP}" | grep -o '"total":[0-9]*' | grep -o '[0-9]*')
  sleep 0.5
done

# 8. 停止监控
curl -s -X POST http://${IP}/api/stop

# 9. 可选：重启烧录器
curl -s -X POST http://${IP}/api/reboot
```

### 8.2 Python 自动化示例

```python
import requests, base64, time

IP = "192.168.1.100"
BIN = "CC2530.bin"

# 1. 检查设备就绪
s = requests.get(f"http://{IP}/api/status").json()
assert s["state"] == "idle", f"设备忙: {s['state']}"

# 2. 上传 BIN
with open(BIN, "rb") as f:
    r = requests.post(f"http://{IP}/api/upload", files={"file": f})
print("上传:", r.json())

# 3. 异步烧录（强制 verify）
r = requests.post(f"http://{IP}/api/burn",
                  json={"filename": BIN, "verify": True})
task = r.json()
print(f"烧录任务 task_id={task['task_id']}, blocks={task['total_blocks']}")

# 4. 轮询进度
while True:
    s = requests.get(f"http://{IP}/api/status").json()
    b = s["burn"]
    print(f"\r烧录进度: {b['percent']}% ({b['current_block']}/{b['total_blocks']})", end="")
    if b["done"]:
        if b["error"]:
            print(f"\n烧录失败: {b['error']}")
            exit(1)
        print("\n烧录成功")
        break
    time.sleep(2)

# 5. 开始监控（自动复位 CC2530，捕获启动日志）
requests.post(f"http://{IP}/api/monitor",
              json={"baud": 115200, "auto_reset": True})

# 6. 轮询获取日志（5 秒后停止）
offset = 0
deadline = time.time() + 5
while time.time() < deadline:
    r = requests.get(f"http://{IP}/api/monitor/buffer?since={offset}")
    d = r.json()
    if d["data"]:
        log = base64.b64decode(d["data"]).decode('utf-8', errors='replace')
        print(log, end='')
        offset = d["total"]
    if d.get("truncated"):
        print(f"\n[警告：缓冲溢出，丢失 {d['missed']} 字节]")
    time.sleep(0.3)

# 7. 停止监控
requests.post(f"http://{IP}/api/stop")
print("\n完成")
```

---

## 9. 监控日志获取两种方式

CC2530 串口日志可通过两种方式获取，根据使用场景选择：

### 9.1 方式 A：SSE 长连接（实时推送，端口 81）

**适用场景**：浏览器实时显示、需要立即响应的交互场景。

**特点**：
- 实时性好，延迟约 200ms（攒 256 字节或 200ms 静默推送）
- 单连接占用一个 SSE 客户端槽位（最多 4 个）
- 数据以 Base64 编码传输
- 自动收到所有事件（monitor_start / monitor_data / monitor_reset / monitor_stop）

**Python 示例**：

```python
import requests, base64, json

with requests.get('http://192.168.1.100:81/', stream=True, timeout=None) as r:
    for line in r.iter_lines():
        if line.startswith(b'data: '):
            msg = json.loads(line[6:])
            if msg['type'] == 'monitor_start':
                print(f"[监控开始 baud={msg['baud']}]")
            elif msg['type'] == 'monitor_data':
                log = base64.b64decode(msg['data']).decode('utf-8', errors='replace')
                print(log, end='')
            elif msg['type'] == 'monitor_reset':
                print("\n[CC2530 已复位，日志区清空]")
            elif msg['type'] == 'monitor_stop':
                print("\n[监控停止]")
                break
```

### 9.2 方式 B：轮询环形缓冲（断点续传，端口 80）

**适用场景**：Agent 自动化、需要完整日志（不丢数据）、网络不稳定场景。

**特点**：
- 支持断点续传（`since` 参数）
- 缓冲大小 8192 字节，超出后旧数据被覆盖（`truncated=true` 提示）
- 可限制单次返回字节数（`max_bytes` 参数）
- 流式分块输出，避免大响应导致堆 OOM
- Agent 可在 8192 字节未满前拉取，避免数据丢失

**Python 示例**：

```python
import requests, base64, time

offset = 0
while True:
    r = requests.get(f'http://192.168.1.100/api/monitor/buffer?since={offset}')
    data = r.json()
    if data['data']:
        log = base64.b64decode(data['data']).decode('utf-8', errors='replace')
        print(log, end='')
        offset = data['total']  # 更新偏移到最新
    if data.get('truncated'):
        print(f"\n[警告：缓冲溢出，丢失 {data['missed']} 字节]")
        offset = data['total']  # 跳过丢失部分
    time.sleep(0.5)  # 轮询间隔
```

### 9.3 两种方式对比

| 特性 | SSE 长连接 | 轮询环形缓冲 |
|---|---|---|
| 端口 | 81 | 80 |
| 实时性 | 高（~200ms） | 取决于轮询间隔 |
| 数据完整性 | 依赖连接稳定性 | 断点续传，可保证完整 |
| 客户端数限制 | 最多 4 个 | 无限制（HTTP 短连接） |
| 历史数据 | 无（仅推送新数据） | 有（最近 8192 字节） |
| 网络中断恢复 | 需重连，可能丢数据 | `since` 参数续传 |
| 适用场景 | 浏览器实时显示 | Agent 自动化 |

> **推荐**：Agent 优先使用方式 B（轮询），可保证日志完整性；浏览器使用方式 A（SSE），实时性好。

---

## 10. 关键限制

| 限制项 | 值 | 说明 |
|---|---|---|
| **同步烧录 HTTP 超时** | 建议 600 秒 | 现已强制异步，同步模式已移除；保留此值供旧客户端参考 |
| **异步烧录返回码** | 202 Accepted | 立即返回 `task_id`，轮询 `/api/status` 跟踪进度 |
| **监控环形缓冲大小** | 8192 字节 | 超出后旧数据被覆盖，`truncated=true` 提示，`missed` 字段报告丢失字节数 |
| **监控单次推送阈值** | 256 字节或 200ms | 攒够 256 字节或 200ms 静默时推送，降低 SSE 频率 |
| **监控单次 loop 读取** | 128 字节 | 防止 `Serial.available()` 长时间占用 CPU 导致 HTTP 假死 |
| **SSE 最大客户端** | 4 个 | 第 5 个连接被拒绝并关闭 |
| **BIN 文件大小** | ≤ 256KB | CC2530F256 Flash 大小；LittleFS 1MB 分区可用约 700KB |
| **波特率范围** | 9600 - 230400 | `/api/monitor` 的 `baud` 参数有效范围 |
| **WiFi 频段** | 仅 2.4GHz | ESP8266 硬件限制，不支持 5GHz |
| **WiFi 连接超时** | 8 秒 | `/api/wifi/connect` 同步等待，超时回退 AP 模式 |
| **WiFi SSID 长度** | ≤ 32 字节 | 802.11 标准限制 |
| **OTA 升级耗时** | 10-30 秒 | 升级期间 HTTP/SSE 暂不可用，自动重启后恢复 |
| **task_id 单调递增** | 从 1 开始 | 每次成功排队 `/api/burn` 递增，用于跟踪任务 |
| **烧录块大小** | 512 字节 | CC2530 Flash 页大小，256KB = 512 块 |
| **烧录进度推送间隔** | 每 16 块 | 约 3% 进度更新一次，避免 SSE 过频 |
| **CC2530 NV 区域** | 最后 4KB | `/api/nvreset` 仅清除这部分，保留固件 |
| **CC2530 Flash 大小** | 256KB (0x40000) | CC2530F256，BIN 文件自动填充 0xFF 到 256KB |

### 10.1 烧录错误处理

烧录失败时，`/api/status` 的 `burn.error` 字段和 SSE `burn_progress.error` 字段会包含错误信息。常见错误：

| 错误信息 | 原因 | 排查 |
|---|---|---|
| `chip not detected` | CC2530 未连接或线接错 | 检查 DD/DC/RESET 三根线 |
| `XOSC timeout` | 外部晶振未起振 | CC2530 硬件问题，更换模块 |
| `verify failed at block N` | 第 N 块校验失败 | DD 线接触不良或 Flash 寿命耗尽 |
| `file not found: xxx` | 文件不存在 | 检查 `/api/files` 列表 |

### 10.2 性能参考

| 操作 | 耗时 | 说明 |
|---|---|---|
| 上传 256KB BIN | 5-10 秒 | 取决于 WiFi 信号质量 |
| 烧录 256KB BIN | 约 90 秒 | 含强制校验 |
| 清除配网（nvreset） | 约 2 分钟 | 读取+擦除+写回 |
| 备份固件（backup） | 约 1 分钟 | 仅读取+保存 |
| WiFi 扫描 | 3-5 秒 | 阻塞调用 |
| WiFi 连接 | 最多 8 秒 | 超时回退 AP |

---

## 附录：路由注册源码参考

所有 HTTP 路由在 `src/CCLoader.ino` 的 `initHttpRoutes()` 函数中注册（第 1580-1621 行），OTA 升级通过 `httpUpdater.setup(&server, "/update")` 挂载（第 1651 行）。SSE 服务器在 `setup()` 中通过 `sseServer.begin()` 启动（第 1652 行）。
