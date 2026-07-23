# 更新日志

本项目所有显著变更均记录于此文件。

本项目基于 RedBearLab CCLoader（2013-2014 年原始 CCLib 协议实现）改造，
**自 2026-07-20 起为 WebUI 烧录+监控一体机版本**。以下仅记录 WebUI 改造以来的变更，
原始 CCLib 协议代码（write_debug_byte ~ RunDUP）保持不变，不再追溯。

格式遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

---

## [Unreleased]

### 变更
- **监控模块通用化**：`resetCC2530()` 移除调试专用的 DD/DC 拉低操作，只操作 RESETn 一根线
  （拉低 DD/DC 会让 CC Debug 接口进入非标准电平，对运行中的应用固件有干扰）
- **`auto_reset` 默认改为 `false`（非侵入式监听）**：后端 `/api/monitor` 默认不再自动复位
  目标设备；前端 checkbox 取消默认勾选。如需捕获从 main() 开始的启动日志，请显式勾选
  或在 API 调用中传 `"auto_reset": true`
- 监控中手动复位 CC2530 时保留 `g_monitor_ring_total` / `g_monitor_bytes_total` 累计计数，
  保证 Agent `/api/monitor/buffer?since=N` 断点续传的 offset 语义单调递增

### 修复
- 烧录中启动监控前端提示不友好：409 `busy` 现映射为"设备忙（烧录中或监控中），
  请先停止当前操作"

### 版本号
- 固件版本号 v1.1 → **v1.2**

---

## [1.1] - 2026-07-23

### 新增
- **OTA 分体固件烧录模式**：烧录页支持"单文件 / OTA 分体固件"切换，OTA 模式下选择
  Boot.hex (0x0000~0x07FF) + App.hex (0x0800+) 两个文件，前端合并为 256KB BIN 后
  复用现有 `/api/upload` + `/api/burn` 链路（方案 A：前端合并，后端零改动）
- **烧录完成重启烧录器选项**：烧录卡片增加 `reboot-burner-check` 复选框，勾选后烧录完成
  自动调用 `/api/reboot` 重启 ESP8266
- **API 拒绝 .hex 直传**：`/api/upload` 收到 .hex 时返回 400 + `hint` 字段（含完整 Intel HEX
  解析 + 256KB 填充算法说明），避免 AI 把 .hex 当 BIN 烧录导致 CC2530 异常
- **NTP 网络授时（北京时间 UTC+8）**：WiFi 连接成功后 `configTime`，LittleFS 文件时间戳
  不再显示 1970；`/api/status` 新增 `time` 字段方便判断是否已授时
- 帮助页更新：API 用法补充 .hex 不支持提示和 reboot 示例；API 端点表补全
  `/api/config`、`DELETE /api/files/{name}`；SSE 事件补全 `wifi_connected`/`wifi_connect_failed`；
  FAQ 新增 NTP 授时、.hex 报错、烧录完成重启选项说明

### 变更
- **移除烧录完成后自动跳转监控页**的行为，改为由用户手动切换
- `hex2bin` 重构为 `parseHexToMap` + `mapToBin` 两个函数，便于 OTA 分体固件合并复用
- `handleBurn` 移除同步分支，一律返回 202 + `task_id`（强制异步烧录）
- `/api/monitor/buffer` 改为 chunked 流式输出，新增 `max_bytes` 参数（默认 4096）
- 监控 loop 单次读取限制 128 字节，SSE 推送阈值改为 200ms 或 256 字节
- 烧录循环每 16 块 yield + `handleClient`，避免饿死 HTTP 调度

### 修复
- **AI 调用 API 假死/反应慢**：监控 loop 长时间占用 CPU 导致 `/api/status` 超时；
  同步烧录阻塞 HTTP；monitor buffer 一次性返回 8KB base64 导致堆碎片化
- `settings` 标签页未闭合的 `</section>` 导致 `help` 页面嵌套渲染异常
- `agent_demo.py` SSE 无超时，ESP8266 断电后 Python 端永久阻塞

---

## [1.0] - 2026-07-20

### 新增
- **基于 CCLib 的 CC2530 烧录工具 WebUI 化**：ESP8266 (NodeMCU) 作为 WiFi 烧录+监控一体机
- **WiFi 配网**：AP 模式 captive portal + STA 模式切换，支持扫描周围网络
- **文件上传**：支持 .bin 直接烧录和 .hex 自动转换（浏览器端 hex2bin，填充到 256KB）
- **异步烧录**：`POST /api/burn` 立即返回 `task_id`，进度通过 SSE 实时推送
- **串口监控**：Serial 切换到 CC2530 波特率，接收 P0_3 日志并通过 SSE 推送
- **自动复位 CC2530**：进入监控时拉低 RESETn 10ms，捕获从 main() 开始的启动日志
- **Agent 友好 API**：异步烧录、`/api/monitor/buffer?since=N` 断点续传、8KB 环形缓冲
- **ESP8266HTTPUpdateServer**：支持 WiFi 远程 OTA 升级固件
- **帮助页面**：接线图（ASCII art）、首次使用流程、API 用法示例
- **内嵌静态资源**：`web_assets.h` 由 `tools/gen_web_assets.py` 从 `data/` 生成，
  OTA 升级固件时一并更新 WebUI，无需 uploadfs

### 技术规格
- 硬件：NodeMCU ESP8266 + CC2530 模块（5 根线连接）
- 后端：ESP8266WebServer (端口 80) + 原生 WiFiServer SSE (端口 81)
- 前端：原生 HTML/CSS/JS，无外部库依赖
- 文件系统：LittleFS
- CC Debug 协议：GPIO bit-bang（GPIO5/RESETn, GPIO4/DC, GPIO12/DD）

---

[Unreleased]: https://github.com/wedone/CCLoader/compare/v1.1...HEAD
[1.1]: https://github.com/wedone/CCLoader/compare/v1.0...v1.1
[1.0]: https://github.com/wedone/CCLoader/releases/tag/v1.0
