# Sniffer 非阻塞端口重构设计

## 背景

ESP32-SOLO-1 上抓包时设备进入"死机"状态：WebUI 不能刷新、`/api/stop` 无响应、所有 API 超时。WiFi 仍连通（可 ping 通），仅 HTTP 阻塞。

### 根因

`handleSnifferStream`（`/api/sniffer/stream`）是阻塞式 HTTP handler，进入后死循环推送 chunked 数据。ESP32 WebServer 单线程同步架构下，handler 阻塞期间 `handleClient()` 不执行，无法 accept 新连接，导致端口 80 上所有请求（页面、API、停止指令）全部排队超时。

ESP8266 上同样代码"看似正常"是因为其 `yield()` 会驱动 WiFi/TCP 栈，阻塞 handler 期间仍能处理部分新连接。ESP32 的 WiFi/TCP 在独立 FreeRTOS 任务，`yield()` 不涉及 WebServer 逻辑，阻塞就是真阻塞。这是平台架构差异，非硬件缺陷。

## 目标

- 抓包期间 WebServer(80)、SSE(81) 全程正常响应
- WebUI 可刷新、`/api/stop` 可停止、`/api/status` 可查询
- sniffer 数据实时推送，延迟与原方案相当
- 同时验证硬件（WiFi/UART/CC2530）无缺陷

## 方案：独立端口非阻塞流（方案 B）

新增 `WiFiServer snifferServer(82)`，与 WebServer(80)、SSE(81) 三端口物理分离。`loop()` 中轮流服务三个 server，sniffer stream 每次循环非阻塞推送一小批数据。

### 架构

```
端口 80  WebServer       同步 handler（页面/API/上传/烧录）
端口 81  SSE Server      非阻塞（状态事件、监控数据）
端口 82  Sniffer Server  非阻塞（chunked 原始字节流）  ← 新增
```

三端口共享同一个 `loop()`，每个 server 每轮 `loop()` 各 `accept()` 一次、各 write 一批。WebServer 永不阻塞。

### 数据流

```
CC2530 P0_3 ──115200──> Serial2 (IO16)
                              │
                              ▼
                    handleSniffing()  ← loop() 每轮调用
                              │
                              ▼
                    64KB 环形缓冲 (g_sniffer_buf)
                              │
                              ▼
                    snifferServer(82).write()  ← loop() 每轮非阻塞推送
                              │
                              ▼
                    浏览器 fetch stream
```

`handleSniffing()` 已存在且在 `loop()` 中调用（STATE_SNIFFING 分支），持续读 Serial2 入环形缓冲。新增逻辑：`loop()` 中若 sniffer 客户端连接，从环形缓冲读一批数据，chunked write 给客户端。

### 关键点

1. **无阻塞 handler**：删除 `/api/sniffer/stream` 路由，sniffer 数据改由 `loop()` 中的 `snifferServerLoop()` 非阻塞推送
2. **单客户端**：同时只允许 1 个 sniffer 客户端（避免多客户端分流数据），与现有逻辑一致
3. **chunked 分帧**：服务端手动构造 `HTTP/1.1 200` + `Transfer-Encoding: chunked` 响应头，后续每批数据按 `<hex>\r\n<data>\r\n` 格式 write
4. **断开检测**：`client.connected()` 返回 false 时关闭连接，sniffer 继续运行（与原方案一致）
5. **前端改动最小**：stream URL 从 `/api/sniffer/stream` 改为 `http://<host>:82/`

### 非目标

- 不改 ZBOSS 协议解析（前端 `parseZbossPackets` 保持不变）
- 不改环形缓冲大小和结构
- 不改 `enterSnifferMode`/`exitSnifferMode` 逻辑
- 不改 SSE(81) 通道

## 实施阶段

### 阶段 1：后端实现 snifferServer(82)

新增内容：
1. 全局变量：`WiFiServer snifferServer(82)`、`WiFiClient snifferClient`、`bool snifferClientActive`
2. `setup()` 中 `snifferServer.begin()`
3. `snifferServerLoop()` 函数：
   - 若无客户端：`snifferServer.accept()`，有新连接则发 chunked 响应头
   - 若有客户端：检查 `connected()`，断开则清理；连接中则从环形缓冲读 ≤1024 字节，chunked write
   - 单次 write 不超过 1024 字节，避免长时间占用 loop()
4. `loop()` 中调用 `snifferServerLoop()`（STATE_SNIFFING 时）
5. `exitSnifferMode()` 中断开 snifferClient

保留内容（暂不删）：
- `handleSnifferStream` 函数和路由保留，但前端不再调用（阶段 3 删除）

### 阶段 2：前端 stream URL 改为端口 82

`data/app.js` 的 `readSnifferStream()`：
- URL 从 `/api/sniffer/stream` 改为 `http://<hostname>:82/`
- 其他逻辑（AbortController、reader.read、parseZbossPackets）不变

### 阶段 3：删除阻塞 handler

确认端口 82 方案工作正常后：
- 删除 `handleSnifferStream` 函数
- 删除 `/api/sniffer/stream` 路由
- 删除 `g_sniffer_client`、`g_sniffer_client_active`（改用 snifferClient/snifferClientActive）

## 风险与缓解

| 风险 | 缓解 |
|------|------|
| 端口 82 被防火墙拦截 | 浏览器 fetch 同一主机的非标准端口通常无问题；若用户环境有限制，文档说明 |
| `loop()` 单次推送数据量过大导致其他 server 饥饿 | 单次 write 限制 ≤1024 字节，`loop()` 高频调度 |
| chunked 响应头格式错误导致浏览器无法解析 | 严格按 HTTP/1.1 chunked 规范，参照原 `handleSnifferStream` 的响应头 |
| 客户端断开但 `connected()` 未及时检测 | 每轮 `loop()` 都检查 `connected()`，write 失败也视为断开 |

## 验证标准

1. 抓包期间 WebUI 可刷新（`GET /` 正常响应）
2. 抓包期间 `/api/status` 正常响应，状态显示"抓包中"
3. 抓包期间点"停止"按钮，`/api/stop` 正常响应，sniffer 停止
4. 抓包数据实时显示在页面表格中，延迟 ≤500ms
5. sniffer 客户端断开后 sniffer 继续运行，可重新连接 stream
6. 串口日志显示 `[SNIFFER] rx/sent` 持续增长，无丢包
