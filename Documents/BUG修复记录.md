# CCLoader BUG 修复记录

本文件记录 CCLoader 项目的历史 BUG、根因分析和修复方案。

> 记录范围：WebUI 改造以来（v1.0，2026-07-20）的修复条目。原始 CCLib 协议代码（`write_debug_byte` ~ `RunDUP`）不在追溯范围内。
>
> 排列顺序：按版本时间倒序，最新版本在前；同一版本内按修复重要性排序。
>
> 源文件参考：
> - `CHANGELOG.md`（版本变更记录）
> - `src/CCLoader.ino`（ESP8266 后端实现）
> - `tools/gen_web_assets.py`（静态资源生成脚本）
> - `data/index.html`、`data/app.js`（前端实现）
> - `examples/agent_demo.py`（Python Agent 示例）

---

## v1.2（2026-07-23）

### BUG-1.2.3 ｜ `gen_web_assets.py` 换行符导致 `sizeof` 与 `_len` 不匹配

- **现象**
  - 编译后 WebUI 页面（`index.html` / `style.css` / `app.js`）偶尔显示异常：
    - 静态资源末尾被截断
    - 浏览器 Console 报 `ERR_CONTENT_LENGTH_MISMATCH`
    - 个别情况下 CSS/JS 解析到一半报语法错误
  - 直接访问 `http://<ip>/style.css` 时，响应头 `Content-Length` 与实际 body 字节数不一致。
- **根因**
  - `tools/gen_web_assets.py` 在 Windows 下生成 `web_assets.h` 时，源文件（`data/*.html`、`data/*.css`、`data/*.js`）的换行符是 `\r\n`（CRLF）。
  - 这些内容以 C++ raw string（`R"(...)"`）或字符数组形式写入头文件后，GCC 在编译期会把字符串字面量中的 `\r\n` 规范化为 `\n`（C/C++ 标准允许的实现行为）。
  - 后端用 `sizeof(WebAssets::index_html)` 作为 `Content-Length`，但 `sizeof` 反映的是源文件中带 `\r` 的字节数（偏大），而实际写入 body 的字节数已不含 `\r`（偏小），二者不一致。
  - 同时 `web_assets.h` 中的 `index_html_len` 等常量也是基于带 `\r` 的原始长度计算，同样偏大。
  - 最终浏览器按 `Content-Length` 等待更多字节，但后端已经发完关闭连接，触发截断/不匹配错误。
- **修复方案**
  - `gen_web_assets.py` 生成头文件时统一去除 `\r`，确保写入头文件的字符串字面量只含 `\n`。
  - `index_html_len` / `style_css_len` / `app_js_len` 改用去 CR 后的字节数（即 GCC 编译后 `sizeof` 的实际值）。
  - 这样 `Content-Length` 头与实际 body 字节数完全一致，避免任何换行符差异。
- **影响版本**：v1.1 及之前（Windows 环境生成的 `web_assets.h`）
- **修复版本**：v1.2
- **相关代码**：`tools/gen_web_assets.py`、`src/web_assets.h`

---

### BUG-1.2.2 ｜ 跨 tailscale 访问 WebUI 大响应截断

- **现象**
  - 通过 Tailscale 虚拟网络远程访问 CCLoader WebUI 时，HTML / CSS / JS 等静态资源加载不完整：
    - `index.html` 渲染到一半就空白
    - `app.js` 报 `Uncaught SyntaxError: Unexpected end of input`
    - 浏览器 Network 面板显示对应请求 `(failed) net::ERR_INCOMPLETE_CHUNKED_ENCODING` 或状态码 200 但内容截断
  - 局域网直连访问完全正常，只有经过 tailscale 转发时复现。
- **根因**
  - Tailscale 默认路径 MTU（PMTU）为 1280 字节（IPv6 最小 MTU，避免分片）。
  - ESP8266 lwIP 默认 TCP MSS = 1460，单 TCP 段加 IP/TCP 头后 IP 包约 1500 字节。
  - 当 ESP8266 发出 1500 字节的 IP 包经过 tailscale 隧道时，外层 IP 头 DF=1（不分片），但 PMTU=1280，导致包被丢弃。
  - 大响应（如 `index.html` ~10KB、`app.js` ~20KB）被切成多个 1500 字节段，其中超过 1280 的段全部丢失，最终客户端收到的内容残缺。
  - 原后端用 `server.send_P()` 一次性发送整个 PROGMEM 数据，无法控制 TCP 段大小。
- **修复方案**
  - 新增 `sendChunked_P(const char* mime, PGM_P data, size_t len)` 函数（`CCLoader.ino` 第 1108-1134 行），核心策略：
    1. **分块发送**：每块 1000 字节，单 TCP 段 IP 包 ≈ 1000 + 40（IP/TCP 头）= 1040 字节 < 1280（tailscale PMTU），无需分片即可通过。
    2. **禁用 Nagle**（`client.setNoDelay(true)`）：防止 lwIP 把多个 1000 字节块合并成超过 PMTU 的大包。
    3. **可靠写入重试**：`client.write()` 可能短写（返回值 < 请求长度），原 `sendContent_P` 短写后丢弃数据导致末尾截断；自己实现的 `while (written < n)` 循环检查返回值并重试，写不进去时 `delay(1)` 等 lwIP 缓冲区腾空。
    4. **`Connection: close`**：响应头显式关闭连接，避免 keep-alive 状态下客户端等待更多数据。
    5. **`setTimeout(5000)`**：增加写超时，防止慢链路下误判失败。
  - `handleRoot()` / `handleCss()` / `handleJs()` 全部改用 `sendChunked_P()`，传入对应的 `*_len` 常量（见 BUG-1.2.3 修复后的字节数）。
- **影响版本**：v1.0、v1.1（经 tailscale 或任何 PMTU < 1500 的链路访问时）
- **修复版本**：v1.2
- **相关代码**：`src/CCLoader.ino` 第 1102-1146 行（`sendChunked_P` 及其调用点）

---

### BUG-1.2.1 ｜ 烧录中启动监控前端提示不友好

- **现象**
  - 设备正在烧录固件时，用户点击 WebUI 的"开始监控"按钮，前端弹出错误提示为原始的 `{"error":"busy"}` 或空白，用户无法理解发生了什么。
  - 通过 API 直接调用 `/api/monitor` 时收到的响应同样只是 `409 {"error":"busy"}`，缺乏可操作的说明。
- **根因**
  - 后端 `handleMonitor()` 在 `g_state != STATE_IDLE` 时直接返回 `409 {"error":"busy"}`（`CCLoader.ino` 第 1351-1354 行），语义正确但信息过于简略。
  - 前端 `data/app.js` 的 fetch 错误处理只 `alert(err.message)` 或显示状态码，没有对 409 + `busy` 做语义映射，用户看到的是英文 raw 错误或无意义的数字。
- **修复方案**
  - 前端拦截 409 响应，解析 JSON 中的 `error == "busy"`，映射为中文友好提示：
    > "设备忙（烧录中或监控中），请先停止当前操作"
  - 同时提示用户可以点击"停止"按钮终止当前任务后再重试。
- **影响版本**：v1.0、v1.1
- **修复版本**：v1.2
- **相关代码**：`data/app.js`（监控按钮 click handler 的错误分支）

---

### BUG-1.2.0 ｜ `resetCC2530()` 干扰运行中的固件（监控模块通用化）

- **现象**
  - 在监控模式下手动点击"复位 CC2530"按钮后，部分应用固件行为异常：
    - Zigbee 协调器掉线，已配网的终端设备失联
    - 串口日志输出乱码或停止
    - 偶发 CC2530 完全无响应，需要重新上电
  - 仅在监控模式下手动复位时复现，烧录流程中的复位（`debug_init` / `RunDUP`）无此问题。
- **根因**
  - 原 `resetCC2530()` 实现复用了调试流程的引脚操作：
    ```cpp
    digitalWrite(DD, LOW);
    digitalWrite(DC, LOW);
    digitalWrite(RESET, LOW);
    delay(10);
    digitalWrite(RESET, HIGH);
    delay(10);
    ```
  - 即在拉低 `RESETn` 的同时拉低了 `DD`（GPIO12）和 `DC`（GPIO4）。
  - `DD` 和 `DC` 是 CC2530 的调试专用引脚（Debug Data / Debug Clock），拉低后会让 CC Debug 接口进入非标准电平状态。
  - 当 `RESETn` 释放、CC2530 重新启动时，由于 `DD`/`DC` 仍为低，CC2530 可能误进入 debug 模式或调试寄存器被意外改写，导致应用固件外设（RF / UART / GPIO）行为异常。
  - 监控场景只需要复位目标芯片，不需要进入 CC Debug 模式，拉低 `DD`/`DC` 是多余的副作用。
- **修复方案**
  - `resetCC2530()` 改为只操作 `RESETn` 一根线（`CCLoader.ino` 第 1027-1032 行）：
    ```cpp
    void resetCC2530() {
      digitalWrite(RESET, LOW);
      delay(10);
      digitalWrite(RESET, HIGH);
      delay(10);
    }
    ```
  - 移除对 `DD`/`DC` 的任何操作，避免干扰应用固件。
  - 同步将 `auto_reset` 默认值改为 `false`（非侵入式监听），用户显式勾选才会自动复位目标设备。
  - 监控中手动复位时保留 `g_monitor_ring_total` / `g_monitor_bytes_total` 累计计数，保证 Agent `/api/monitor/buffer?since=N` 断点续传的 offset 语义单调递增。
- **影响版本**：v1.0、v1.1（监控中手动复位时）
- **修复版本**：v1.2
- **相关代码**：`src/CCLoader.ino` 第 1023-1032 行（`resetCC2530`），第 1377-1399 行（`handleResetCC2530`）

---

## v1.1（2026-07-23）

### BUG-1.1.3 ｜ `agent_demo.py` SSE 无超时导致永久阻塞

- **现象**
  - 运行 `examples/agent_demo.py` 监听 SSE 事件流时，如果 ESP8266 断电或网络中断：
    - Python 端 `requests.get(..., stream=True)` 永久阻塞在 `iter_lines()`，不抛异常也不退出。
    - Ctrl+C 中断后再次运行，端口被占用或状态不一致。
    - Agent 自动化场景下会"假活"——进程在但永不产出。
- **根因**
  - 原代码：
    ```python
    resp = requests.get(SSE_URL, stream=True)
    for line in resp.iter_lines():
        ...
    ```
  - `requests.get` 默认 `timeout=None`（无限等待），TCP 连接断开后底层 socket 不一定立即通知应用层（取决于 OS 的 keepalive 设置）。
  - 当 ESP8266 突然断电时，没有发送 TCP FIN 包，Python 端 socket 一直等待数据，没有超时机制触发异常。
- **修复方案**
  - 为 `requests.get` 添加 `timeout` 参数：
    ```python
    resp = requests.get(SSE_URL, stream=True, timeout=(5, 30))
    ```
    - 连接超时 5 秒，读取超时 30 秒（SSE 流式读取每次 `iter_lines` 最多等 30 秒）。
  - 捕获 `requests.exceptions.Timeout` / `ConnectionError`，重连退避（指数退避，最大 60 秒）。
- **影响版本**：v1.0（agent_demo.py 引入时）
- **修复版本**：v1.1
- **相关代码**：`examples/agent_demo.py`

---

### BUG-1.1.2 ｜ `settings` 标签页未闭合的 `</section>` 导致 help 页面嵌套渲染异常

- **现象**
  - 打开 WebUI 的"帮助"标签页时，页面布局错乱：
    - 帮助内容被嵌套在"设置"标签页的卡片内
    - "设置"标签页的样式泄漏到帮助页
    - 切换其他标签页时 DOM 结构异常，部分按钮失效
  - 直接访问 `data/index.html`（独立打开）无此问题，仅在内嵌到 ESP8266 提供的页面时复现。
- **根因**
  - `data/index.html` 中 `settings` 标签页的 `<section class="tab-content" id="settings-tab">` 缺少闭合的 `</section>` 标签。
  - 浏览器 HTML 解析器的容错机制会把后续的 `help` 标签页 `<section>` 嵌套进 `settings-tab` 内部，导致 DOM 树结构与预期不符。
  - CSS 选择器 `.tab-content > .tab-content` 等基于直接子元素的样式规则因此失效或错误应用。
- **修复方案**
  - 在 `data/index.html` 中 `settings-tab` 的最后一个子元素后补全 `</section>` 标签。
  - 用 HTML 验证器（如 W3C Validator）扫描全部页面，确认无其他未闭合标签。
- **影响版本**：v1.0
- **修复版本**：v1.1
- **相关代码**：`data/index.html`（settings 标签页结构）

---

### BUG-1.1.1 ｜ AI 调用 API 假死 / 反应慢

- **现象**
  - 通过 AI Agent（如 `agent_demo.py` 或外部 LLM 工具）调用 CCLoader API 时出现多种"假死"：
    1. **`/api/status` 超时**：Agent 轮询状态接口，10 秒内无响应，HTTP 客户端超时。
    2. **烧录请求阻塞 HTTP**：调用 `POST /api/burn` 后，整个 Web 服务在烧录期间（约 90 秒）完全无响应，其他 API 全部超时。
    3. **`/api/monitor/buffer` 返回 8KB base64 时 OOM**：ESP8266 堆约 50KB，单次响应分配 ~22KB 临时字符串（8KB 原始 → 11KB base64 + JSON 拼接），偶发分配失败导致连接断开。
  - 局域网内浏览器访问偶发卡顿，但 Agent 高频轮询时极易触发。
- **根因**
  - **问题 1：监控 loop 长时间占用 CPU**
    - `handleMonitoring()` 原实现：
      ```cpp
      while (Serial.available()) {
        g_monitor_buf[g_monitor_len++] = Serial.read();
        ...
      }
      ```
    - CC2530 在 115200 波特率下高速输出日志时，`Serial.available()` 持续为真，`while` 循环不退出，`server.handleClient()` 长时间得不到调度，导致 `/api/status` 等接口超时。
  - **问题 2：同步烧录阻塞 HTTP**
    - `handleBurn()` 原实现支持 `?async=1` 参数，未传时走同步分支：在 HTTP handler 内直接调用 `burnFromLittleFS()`，整个烧录过程（256KB ≈ 90 秒）占用主 loop，HTTP 服务完全停摆。
    - Agent 不传 `async=1` 时即触发阻塞。
  - **问题 3：monitor buffer 一次性返回 8KB base64**
    - `handleMonitorBuffer()` 原实现把环形缓冲全部读出，base64 编码后拼接到 JSON 字符串：
      ```cpp
      String json = "{...\"data\":\"" + base64Encode(buf, 8192) + "\"}";
      server.send(200, "application/json", json);
      ```
    - 单次堆分配：8KB 原始 + 11KB base64 + 22KB JSON ≈ 41KB，ESP8266 50KB 堆极易碎片化或 OOM。
- **修复方案**
  - **修复 1：监控 loop 限制单次读取**
    - `handleMonitoring()` 增加 `MAX_READ_PER_LOOP = 128` 上限（`CCLoader.ino` 第 1078-1087 行），单次 loop 最多读 128 字节（≈ 1ms @ 115200bps），保证 `loop()` 每秒可调度数百次 HTTP。
    - SSE 推送阈值从 50ms / 64 字节改为 200ms / 256 字节，降低堆压力 4 倍。
  - **修复 2：强制异步烧录**
    - `handleBurn()` 移除同步分支（`CCLoader.ino` 第 1294-1328 行），一律返回 `202 + task_id`，烧录在 `loop()` 中通过 `g_burn_pending` 标志异步执行。
    - `burnFromLittleFS()` 内部每 16 块（约 0.8-1.6 秒）调用 `server.handleClient()` + `sseLoop()` + `yield()`，保持 HTTP 可响应。
    - 原 32 块间隔在偶发慢块时可能踩 ESP8266 软件 WDT（~3.2s）边界，改为 16 块更安全。
  - **修复 3：monitor buffer 改为 chunked 流式输出**
    - `handleMonitorBuffer()` 改用 `CONTENT_LENGTH_UNKNOWN` + 分块 `sendContent()`（`CCLoader.ino` 第 1407-1478 行）：
      - 先发送 JSON 头部字段（`total` / `offset` / `bytes` / `truncated`）
      - 然后每 768 字节原始数据 → 1024 字节 base64 字符，分块发送
      - 最后发送 `"}` 闭合 JSON
    - 单次堆分配 < 1.5KB，彻底解决 OOM 问题。
    - 新增 `max_bytes` 参数（默认 4096），允许 Agent 限制单次返回字节数。
- **影响版本**：v1.0
- **修复版本**：v1.1
- **相关代码**：
  - `src/CCLoader.ino` 第 1074-1096 行（`handleMonitoring`）
  - `src/CCLoader.ino` 第 1294-1328 行（`handleBurn`）
  - `src/CCLoader.ino` 第 1407-1478 行（`handleMonitorBuffer`）
  - `src/CCLoader.ino` 第 823-832 行（烧录循环中的 `handleClient` 调度）

---

## 修复统计

| 版本 | 发布日期 | 修复数量 | 主要类别 |
|------|----------|----------|----------|
| v1.2 | 2026-07-23 | 4 | 网络传输兼容性、前端体验、监控模块通用化 |
| v1.1 | 2026-07-23 | 3 | API 响应性、HTML 结构、Python 客户端健壮性 |

> v1.0（2026-07-20）为 WebUI 改造首版，所有问题在该版本引入，v1.1/v1.2 陆续修复。
