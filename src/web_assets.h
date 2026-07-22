// 自动生成 - 不要手动修改
// 由 tools/gen_web_assets.py 从 data/ 目录生成
// OTA 升级固件时一并更新，无需 uploadfs
#pragma once
#include <Arduino.h>

namespace WebAssets {

// index.html (16140 bytes, text/html)
const char index_html[] PROGMEM = R"=====(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>CCLoader WebUI</title>
  <link rel="stylesheet" href="/style.css">
</head>
<body>
  <header>
    <div class="title">CCLoader WebUI</div>
    <div class="status-bar">
      <span id="state-badge" class="badge idle">空闲</span>
      <span id="ip-info">IP: -</span>
      <span id="ws-state">未连接</span>
    </div>
  </header>

  <nav class="tabs">
    <button class="tab-btn active" data-tab="burn">烧录</button>
    <button class="tab-btn" data-tab="monitor">监控</button>
    <button class="tab-btn" data-tab="settings">设置</button>
    <button class="tab-btn" data-tab="help">帮助</button>
  </nav>

  <main>
    <!-- 烧录页 -->
    <section id="tab-burn" class="tab-content active">
      <h2>烧录 CC2530</h2>

      <div class="card">
        <h3>上传固件</h3>
        <div class="burn-mode-switch">
          <label><input type="radio" name="burn-mode" value="single" checked> 单文件</label>
          <label><input type="radio" name="burn-mode" value="ota"> OTA 分体固件</label>
        </div>

        <div id="single-mode" class="burn-mode-panel">
          <input type="file" id="file-input" accept=".bin,.hex">
          <button id="upload-btn" class="btn">上传</button>
          <div id="upload-progress" class="progress-text"></div>
          <div class="hint">支持 .bin（直接烧录）和 .hex（自动转换为 BIN，填充到 256KB）</div>
        </div>

        <div id="ota-mode" class="burn-mode-panel" style="display:none;">
          <div class="ota-file-slot">
            <label>① OTA Bootloader (.hex)</label>
            <input type="file" id="ota-boot-file" accept=".hex">
            <span id="ota-boot-info" class="file-meta"></span>
          </div>
          <div class="ota-file-slot">
            <label>② 应用固件 (.hex)</label>
            <input type="file" id="ota-app-file" accept=".hex">
            <span id="ota-app-info" class="file-meta"></span>
          </div>
          <div id="ota-merge-preview" class="ota-preview"></div>
          <button id="ota-merge-upload-btn" class="btn primary" disabled>合并并上传</button>
          <div id="ota-merge-progress" class="progress-text"></div>
          <div class="hint">CC2530 OTA 固件分 Bootloader (0x0000~0x07FF) + 应用 (0x0800 起)，前端合并为一个 256KB BIN 后复用现有烧录链路</div>
        </div>
      </div>

      <div class="card">
        <h3>已上传固件</h3>
        <div id="file-list"></div>
      </div>

      <div class="card">
        <h3>烧录</h3>
        <div>选中: <span id="selected-file">未选择</span></div>
        <label><input type="checkbox" id="verify-check"> 烧录后校验</label>
        <button id="burn-btn" class="btn primary">开始烧录</button>

        <div class="progress-container">
          <div class="progress-bar">
            <div id="burn-progress-bar" class="progress-fill"></div>
          </div>
          <div id="burn-progress-text" class="progress-text">0%</div>
        </div>

        <div class="log-area" id="burn-log"></div>
      </div>
    </section>

    <!-- 监控页 -->
    <section id="tab-monitor" class="tab-content">
      <h2>串口监控</h2>

      <div class="card">
        <label>波特率:
          <select id="baud-select">
            <option value="9600">9600</option>
            <option value="19200">19200</option>
            <option value="38400">38400</option>
            <option value="57600">57600</option>
            <option value="115200" selected>115200</option>
            <option value="230400">230400</option>
          </select>
        </label>
        <label><input type="checkbox" id="auto-reset-check" checked> 开始时自动复位 CC2530</label>
        <button id="monitor-start-btn" class="btn primary">开始监控</button>
        <button id="monitor-stop-btn" class="btn" disabled>停止</button>
        <button id="reset-cc-btn" class="btn" disabled>复位 CC2530</button>
        <div class="hint">自动复位 = 进入监控时拉低 RESETn 10ms，捕获从 main() 第一行开始的完整启动日志</div>
      </div>

      <div class="card">
        <div class="toolbar">
          <button id="pause-btn" class="btn" disabled>暂停</button>
          <button id="clear-btn" class="btn" disabled>清空</button>
          <button id="download-btn" class="btn" disabled>下载</button>
          <input type="text" id="search-input" placeholder="搜索过滤..." disabled>
        </div>

        <div id="monitor-log" class="log-area"></div>

        <div class="status-bar">
          <span>接收: <span id="bytes-received">0</span> 字节</span>
          <span id="monitor-state">未开始</span>
        </div>
      </div>
    </section>

    <!-- 设置页 -->
    <section id="tab-settings" class="tab-content">
      <h2>设置</h2>

      <div class="card">
        <h3>WiFi 配网</h3>
        <div id="wifi-mode-hint" class="hint"></div>
        <div class="wifi-scan-row">
          <button id="wifi-scan-btn" class="btn">扫描网络</button>
          <span id="wifi-scan-status" class="progress-text"></span>
        </div>
        <div id="wifi-list" class="wifi-list"></div>
        <label>SSID: <input type="text" id="wifi-ssid" placeholder="点上方扫描或手动输入"></label>
        <label>密码: <input type="password" id="wifi-password" placeholder="留空表示开放网络"></label>
        <button id="wifi-connect-btn" class="btn primary">连接</button>
        <div id="wifi-connect-status" class="progress-text"></div>
      </div>

      <div class="card">
        <h3>默认监控</h3>
        <label>波特率:
          <select id="default-baud-select">
            <option value="9600">9600</option>
            <option value="19200">19200</option>
            <option value="38400">38400</option>
            <option value="57600">57600</option>
            <option value="115200" selected>115200</option>
            <option value="230400">230400</option>
          </select>
        </label>
        <button id="save-monitor-btn" class="btn primary">保存</button>
      </div>

      <div class="card">
        <h3>默认烧录</h3>
        <label><input type="checkbox" id="default-verify-check"> 烧录后校验</label>
        <button id="save-burn-btn" class="btn primary">保存</button>
      </div>

      <div class="card">
        <h3>设备信息</h3>
        <div>固件版本: CCLoader-WebUI v1.0</div>
        <div>运行时长: <span id="uptime">-</span></div>
        <div>WiFi 信号: <span id="rssi">-</span> dBm</div>
        <div>IP 地址: <span id="device-ip">-</span></div>
        <button id="reboot-btn" class="btn danger">重启 ESP8266</button>
      </div>
    </section>

    <!-- 帮助页 -->
    <section id="tab-help" class="tab-content">
      <h2>帮助</h2>

      <div class="card">
        <h3>接线方法</h3>
        <div class="hint">NodeMCU ESP8266 与 CC2530 模块共需要 5 根线（含共地）</div>
        <table class="pin-table">
          <thead>
            <tr><th>ESP8266 GPIO</th><th>NodeMCU 丝印</th><th>方向</th><th>CC2530 引脚</th><th>用途</th></tr>
          </thead>
          <tbody>
            <tr><td>GPIO5</td><td>D1</td><td>→</td><td>Pin 7 (RESETn)</td><td>CC Debug 复位</td></tr>
            <tr><td>GPIO4</td><td>D2</td><td>→</td><td>Pin 3 (DC)</td><td>CC Debug 时钟</td></tr>
            <tr><td>GPIO12</td><td>D6</td><td>↔</td><td>Pin 4 (DD)</td><td>CC Debug 数据</td></tr>
            <tr><td>GPIO3</td><td>RX</td><td>←</td><td>P0_3 (UART0 TX)</td><td>监控串口日志</td></tr>
            <tr><td>GND</td><td>GND</td><td>—</td><td>GND</td><td>共地（必须）</td></tr>
          </tbody>
        </table>
        <pre class="ascii-art">   NodeMCU ESP8266                     CC2530 模块
  ┌────────────────┐                ┌─────────────────┐
  │            3V3 ├───────────────►│ VCC             │
  │            GND │◄──────────────►│ GND             │
  │  D1 (GPIO5)    ├───────────────►│ Pin 7 RESETn    │
  │  D2 (GPIO4)    ├───────────────►│ Pin 3 DC        │
  │  D6 (GPIO12)   │◄──────────────►│ Pin 4 DD        │
  │  RX (GPIO3)    │◄───────────────│ P0_3 UART0 TX   │
  └────────────────┘                └─────────────────┘</pre>
        <div class="hint">CC2530 的 3.3V 可从 NodeMCU 的 3V3 引脚取，电流 &lt; 50mA</div>
      </div>

      <div class="card">
        <h3>首次使用流程</h3>
        <ol class="step-list">
          <li>按上表接好 5 根线（CC2530 模块先不接 3V3 电源）</li>
          <li>USB 接 NodeMCU 到电脑，首次烧录固件 + 上传 LittleFS：<br>
            <code>pip install platformio</code><br>
            <code>python -m platformio run -t upload --upload-port COM5</code><br>
            <code>python -m platformio run -t uploadfs --upload-port COM5</code>
          </li>
          <li>烧录时 CC2530 的 TX 线（P0_3 → RX）要拔掉，避免干扰 ESP8266 下载</li>
          <li>烧完后插回 TX 线，给 CC2530 通电</li>
          <li>手机/电脑连 WiFi <code>CCLoader-Setup</code>（首次无密码）</li>
          <li>浏览器访问 <code>http://192.168.4.1/</code>，在"设置"页配 WiFi</li>
          <li>连上 WiFi 后，访问 ESP8266 的新 IP 即可使用</li>
        </ol>
      </div>

      <div class="card">
        <h3>API 用法（自动化调用）</h3>
        <div class="hint">无鉴权，局域网内任意设备可调用。完整流程：上传 BIN → 烧录 → 监控</div>
        <pre class="code-block">#!/bin/bash
IP=10.0.0.147
BIN=DIYRuZRT_256k.bin

# 1. 上传 BIN（multipart/form-data）
curl -s -F "file=@${BIN}" http://${IP}/api/upload
# 返回: {"success":true,"filename":"DIYRuZRT_256k.bin","size":262144}

# 2. 异步烧录（立即返回，烧录在后台执行）
curl -s -X POST "http://${IP}/api/burn?async=1" \
  -H "Content-Type: application/json" \
  -d '{"filename":"DIYRuZRT_256k.bin","verify":true}'
# 返回: {"success":true,"async":true,"task_id":1,"total_blocks":512}

# 3. 轮询状态（每 2 秒）
curl -s http://${IP}/api/status
# 返回: {"state":"burning","task_id":1,"burn_pending":false,
#        "burn":{"percent":50,"current_block":256,"total_blocks":512,...}}

# 4. 烧录完成后启动监控（自动复位 CC2530）
curl -s -X POST http://${IP}/api/monitor \
  -H "Content-Type: application/json" \
  -d '{"baud":115200,"auto_reset":true}'

# 5. 轮询获取日志（断点续传）
curl -s "http://${IP}/api/monitor/buffer?since=0"
# 返回: {"success":true,"total":16,"buffered":16,"offset":0,
#        "data":"<base64>","truncated":false}

# 6. 停止监控
curl -s -X POST http://${IP}/api/stop</pre>
      </div>

      <div class="card">
        <h3>API 端点列表</h3>
        <table class="pin-table">
          <thead>
            <tr><th>方法</th><th>路径</th><th>功能</th></tr>
          </thead>
          <tbody>
            <tr><td>GET</td><td>/api/status</td><td>当前状态（state/burn/monitor/task_id）</td></tr>
            <tr><td>GET</td><td>/api/files</td><td>列出已上传 BIN</td></tr>
            <tr><td>POST</td><td>/api/upload</td><td>上传 BIN（multipart, 字段 file）</td></tr>
            <tr><td>POST</td><td>/api/burn?async=1</td><td>异步烧录（返回 task_id，推荐）</td></tr>
            <tr><td>POST</td><td>/api/burn</td><td>同步烧录（阻塞至完成）</td></tr>
            <tr><td>POST</td><td>/api/monitor</td><td>开始监控（body: {baud,auto_reset}）</td></tr>
            <tr><td>GET</td><td>/api/monitor/buffer?since=N</td><td>获取日志（断点续传）</td></tr>
            <tr><td>POST</td><td>/api/stop</td><td>停止监控</td></tr>
            <tr><td>POST</td><td>/api/reset</td><td>复位 CC2530（GPIO5/RESETn）</td></tr>
            <tr><td>POST</td><td>/api/reboot</td><td>重启 ESP8266</td></tr>
            <tr><td>GET</td><td>/api/wifi/scan</td><td>扫描 WiFi</td></tr>
            <tr><td>POST</td><td>/api/wifi/connect</td><td>连接 WiFi（body: {ssid,password}）</td></tr>
          </tbody>
        </table>
      </div>

      <div class="card">
        <h3>实时事件（SSE 端口 81）</h3>
        <div class="hint">浏览器 EventSource 自动连接，或用 curl -N 监听</div>
        <pre class="code-block">curl -N http://10.0.0.147:81/

# 事件类型：
# {"type":"status","state":"idle|burning|monitoring"}
# {"type":"burn_progress","percent":50,"current_block":256,...}
# {"type":"monitor_start","baud":115200}
# {"type":"monitor_data","data":"<base64>"}
# {"type":"monitor_reset"}
# {"type":"monitor_stop"}</pre>
      </div>

      <div class="card">
        <h3>OTA 远程升级（免拔线）</h3>
        <div class="hint">首次 USB 烧录后，后续固件升级走 WiFi，永久免拔 TX 线。升级时 LittleFS 和 WiFi 配置都保留。</div>
        <ol class="step-list">
          <li>修改源码或页面文件后，编译固件：<code>python -m platformio run</code></li>
          <li>浏览器访问 <code>http://&lt;IP&gt;/update</code>，选择 <code>.pio/build/nodemcuv2/firmware.bin</code> 上传</li>
          <li>等待 10-30 秒，自动重启，固件升级完成</li>
        </ol>
        <div class="hint">命令行方式：</div>
        <pre class="code-block">curl -F "image=@.pio/build/nodemcuv2/firmware.bin" http://10.0.0.147/update
# 返回: Update Success! Rebooting... 自动重启</pre>
        <div class="hint">修改 <code>data/</code> 目录下的页面文件后，需重新生成内嵌资源：</div>
        <pre class="code-block">python tools/gen_web_assets.py  # 重新生成 web_assets.h
python -m platformio run        # 重新编译
curl -F "image=@.pio/build/nodemcuv2/firmware.bin" http://10.0.0.147/update</pre>
        <div class="hint">升级期间 HTTP/SSE 暂不可用（约 10-30 秒），浏览器会断连，重启后自动恢复。</div>
      </div>

      <div class="card">
        <h3>常见问题</h3>
        <div class="faq-item">
          <div class="faq-q">Q: 烧录 ESP8266 时报 "Timed out waiting for packet header"？</div>
          <div class="faq-a">A: CC2530 的 TX 线干扰了 ESP8266 GPIO3 (RX)。烧录前先拔掉 CC2530 P0_3 → ESP8266 RX 这根线，烧完再插回。</div>
        </div>
        <div class="faq-item">
          <div class="faq-q">Q: 监控收不到 CC2530 日志？</div>
          <div class="faq-a">A: 1) 检查 TX 线是否插好；2) 确认 CC2530 已通电；3) 试试不同波特率（115200/57600/9600）；4) 点"复位 CC2530"触发启动日志。</div>
        </div>
        <div class="faq-item">
          <div class="faq-q">Q: 烧录 CC2530 失败？</div>
          <div class="faq-a">A: 1) 检查 D1/D2/D6 三根线是否接对；2) 确认 CC2530 GND 与 NodeMCU 共地；3) CC2530 需通电（3.3V）。</div>
        </div>
        <div class="faq-item">
          <div class="faq-q">Q: 忘了 WiFi 密码或连不上？</div>
          <div class="faq-a">A: 在"设置"页重新扫描+连接。若 WebUI 都进不去，删除 LittleFS 中的 /config.json 或重新烧录固件可重置。</div>
        </div>
      </div>
    </section>
  </main>

  <script src="/app.js"></script>
</body>
</html>

)=====";
const size_t index_html_len = 16140;

// style.css (9164 bytes, text/css)
const char style_css[] PROGMEM = R"=====(
/* CCLoader WebUI - 暗色主题响应式样式 */

:root {
  --bg: #1a1d23;
  --bg-card: #252930;
  --bg-input: #2d323b;
  --border: #3a3f4a;
  --text: #e0e0e0;
  --text-muted: #8a8f9a;
  --primary: #4a9eff;
  --primary-hover: #3a8eef;
  --success: #4caf50;
  --warning: #ff9800;
  --danger: #f44336;
  --monitor: #00bcd4;
}

* { box-sizing: border-box; margin: 0; padding: 0; }

body {
  font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
  background: var(--bg);
  color: var(--text);
  line-height: 1.5;
  min-height: 100vh;
}

header {
  background: var(--bg-card);
  padding: 12px 20px;
  display: flex;
  justify-content: space-between;
  align-items: center;
  border-bottom: 1px solid var(--border);
  flex-wrap: wrap;
  gap: 8px;
}

.title { font-size: 18px; font-weight: 600; }

.status-bar {
  display: flex;
  gap: 16px;
  font-size: 13px;
  color: var(--text-muted);
  align-items: center;
  flex-wrap: wrap;
}

.badge {
  padding: 2px 10px;
  border-radius: 12px;
  font-size: 12px;
  font-weight: 500;
}
.badge.idle    { background: var(--success); color: #fff; }
.badge.burning { background: var(--warning); color: #fff; }
.badge.monitor { background: var(--monitor); color: #fff; }

.tabs {
  display: flex;
  background: var(--bg-card);
  border-bottom: 1px solid var(--border);
  padding: 0 20px;
}

.tab-btn {
  background: transparent;
  border: none;
  color: var(--text-muted);
  padding: 12px 20px;
  cursor: pointer;
  font-size: 14px;
  border-bottom: 2px solid transparent;
  transition: all 0.2s;
}

.tab-btn:hover { color: var(--text); }
.tab-btn.active {
  color: var(--primary);
  border-bottom-color: var(--primary);
}

main {
  max-width: 900px;
  margin: 0 auto;
  padding: 20px;
}

.tab-content { display: none; }
.tab-content.active { display: block; }

h2 { margin-bottom: 16px; }
h3 { margin-bottom: 12px; font-size: 15px; color: var(--text-muted); }

.card {
  background: var(--bg-card);
  border: 1px solid var(--border);
  border-radius: 6px;
  padding: 16px;
  margin-bottom: 16px;
}

label {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  margin-right: 12px;
  font-size: 14px;
}

input[type="text"],
input[type="password"],
input[type="file"],
select {
  background: var(--bg-input);
  border: 1px solid var(--border);
  color: var(--text);
  padding: 6px 10px;
  border-radius: 4px;
  font-size: 14px;
  font-family: inherit;
}

input[type="text"]:focus,
input[type="password"]:focus,
select:focus {
  outline: none;
  border-color: var(--primary);
}

.btn {
  background: var(--bg-input);
  border: 1px solid var(--border);
  color: var(--text);
  padding: 6px 16px;
  border-radius: 4px;
  cursor: pointer;
  font-size: 14px;
  transition: all 0.2s;
}

.btn:hover:not(:disabled) { background: var(--border); }
.btn:disabled { opacity: 0.5; cursor: not-allowed; }
.btn.primary { background: var(--primary); border-color: var(--primary); color: #fff; }
.btn.primary:hover:not(:disabled) { background: var(--primary-hover); }
.btn.danger { background: var(--danger); border-color: var(--danger); color: #fff; }

.progress-container {
  margin: 12px 0;
  display: flex;
  align-items: center;
  gap: 12px;
}

.progress-bar {
  flex: 1;
  height: 20px;
  background: var(--bg-input);
  border-radius: 10px;
  overflow: hidden;
  border: 1px solid var(--border);
}

.progress-fill {
  height: 100%;
  background: linear-gradient(90deg, #4caf50, #8bc34a);
  width: 0%;
  transition: width 0.3s;
  border-radius: 10px;
}

.progress-text { font-size: 13px; color: var(--text-muted); min-width: 80px; }

.log-area {
  background: #000;
  color: #d4d4d4;
  font-family: Consolas, Monaco, "Courier New", monospace;
  font-size: 12px;
  padding: 10px;
  border-radius: 4px;
  height: 300px;
  overflow-y: auto;
  white-space: pre-wrap;
  word-break: break-all;
  margin-top: 12px;
  border: 1px solid var(--border);
}

.log-line { padding: 1px 0; }
.log-line.error { color: var(--danger); }
.log-line.success { color: var(--success); }

#file-list { font-size: 14px; }
.file-item {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 8px;
  border: 1px solid var(--border);
  border-radius: 4px;
  margin-bottom: 6px;
}
.file-item.selected { border-color: var(--primary); background: rgba(74, 158, 255, 0.1); }
.file-item .file-info { display: flex; gap: 12px; align-items: center; }
.file-item .file-name { cursor: pointer; color: var(--primary); }
.file-item .file-meta { color: var(--text-muted); font-size: 12px; }

/* WiFi 配网 */
.hint { color: var(--text-muted); font-size: 12px; margin-top: 6px; }
.wifi-scan-row { display: flex; gap: 8px; align-items: center; margin-bottom: 8px; }
.wifi-list {
  max-height: 220px;
  overflow-y: auto;
  border: 1px solid var(--border);
  border-radius: 4px;
  margin-bottom: 8px;
}
.wifi-item {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 8px 10px;
  border-bottom: 1px solid var(--border);
  cursor: pointer;
}
.wifi-item:last-child { border-bottom: none; }
.wifi-item:hover { background: var(--bg-input); }
.wifi-item.selected { background: rgba(74, 158, 255, 0.15); }
.wifi-item .wifi-ssid { color: var(--text); }
.wifi-item .wifi-meta { color: var(--text-muted); font-size: 12px; }
.wifi-item .wifi-rssi { font-weight: bold; }
.wifi-item .wifi-rssi.strong { color: var(--success); }
.wifi-item .wifi-rssi.medium { color: var(--warning); }
.wifi-item .wifi-rssi.weak { color: var(--danger); }
.wifi-item .wifi-lock { color: var(--warning); margin-left: 6px; }

.toolbar {
  display: flex;
  gap: 8px;
  margin-bottom: 12px;
  flex-wrap: wrap;
  align-items: center;
}

.toolbar input[type="text"] { flex: 1; min-width: 150px; }

@media (max-width: 600px) {
  header { flex-direction: column; align-items: flex-start; }
  .status-bar { flex-direction: column; align-items: flex-start; gap: 4px; }
  .tabs { padding: 0; }
  .tab-btn { flex: 1; text-align: center; }
  main { padding: 12px; }
  .log-area { height: 200px; }
  .pin-table { font-size: 11px; }
  .pin-table th, .pin-table td { padding: 4px 6px; }
  .ascii-art, .code-block { font-size: 9px; }
}

/* 帮助页样式 */
.pin-table {
  width: 100%;
  border-collapse: collapse;
  margin: 12px 0;
  font-size: 13px;
}
.pin-table th, .pin-table td {
  border: 1px solid var(--border);
  padding: 6px 10px;
  text-align: left;
}
.pin-table th {
  background: var(--bg-input);
  color: var(--text);
  font-weight: 600;
}
.pin-table td:first-child { color: var(--primary); font-family: monospace; }
.pin-table td:nth-child(2) { color: var(--success); font-family: monospace; }

.ascii-art {
  background: #000;
  color: #7ec699;
  font-family: Consolas, Monaco, "Courier New", monospace;
  font-size: 11px;
  padding: 12px;
  border-radius: 4px;
  overflow-x: auto;
  margin: 12px 0;
  border: 1px solid var(--border);
  line-height: 1.3;
}

.code-block {
  background: #000;
  color: #d4d4d4;
  font-family: Consolas, Monaco, "Courier New", monospace;
  font-size: 12px;
  padding: 12px;
  border-radius: 4px;
  overflow-x: auto;
  margin: 12px 0;
  border: 1px solid var(--border);
  line-height: 1.5;
  white-space: pre;
}
.code-block code { color: #569cd6; }

.step-list {
  padding-left: 24px;
  margin: 8px 0;
}
.step-list li {
  margin-bottom: 8px;
  line-height: 1.6;
}
.step-list code, .hint code, .faq-a code {
  background: var(--bg-input);
  color: var(--success);
  padding: 1px 6px;
  border-radius: 3px;
  font-family: Consolas, Monaco, monospace;
  font-size: 12px;
}

.faq-item {
  border-left: 3px solid var(--primary);
  padding: 8px 12px;
  margin-bottom: 12px;
  background: var(--bg-input);
  border-radius: 0 4px 4px 0;
}
.faq-q {
  color: var(--text);
  font-weight: 600;
  margin-bottom: 4px;
  font-size: 13px;
}
.faq-a {
  color: var(--text-muted);
  font-size: 13px;
  line-height: 1.6;
}

/* 烧录模式切换 */
.burn-mode-switch {
  display: flex;
  gap: 16px;
  padding: 6px 0;
  margin-bottom: 12px;
  border-bottom: 1px solid var(--border);
}
.burn-mode-switch label {
  cursor: pointer;
  font-weight: 500;
}
.burn-mode-panel {
  padding-top: 4px;
}
.ota-file-slot {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-bottom: 10px;
  flex-wrap: wrap;
}
.ota-file-slot label {
  min-width: 180px;
  font-weight: 500;
  margin-right: 0;
}
.ota-preview {
  background: var(--bg-input);
  border: 1px solid var(--border);
  border-radius: 4px;
  padding: 10px;
  margin: 8px 0;
  font-family: Consolas, Monaco, monospace;
  font-size: 12px;
  color: var(--text-muted);
  white-space: pre-line;
  line-height: 1.6;
  min-height: 20px;
}
.ota-preview.error { color: var(--danger); }
.ota-preview .addr-ok { color: var(--success); }
.ota-preview .addr-warn { color: var(--warning); }

)=====";
const size_t style_css_len = 9164;

// app.js (36728 bytes, application/javascript)
const char app_js[] PROGMEM = R"=====(
// CCLoader WebUI 前端逻辑
// 使用 SSE (EventSource) 接收实时事件，无外部库依赖

let es = null;
let esReconnectTimer = null;
let monitorPaused = false;
let monitorActive = false;
let monitorBytes = 0;
let monitorBuffer = '';  // 不完整行缓冲
let selectedFile = '';

// ===== 工具函数 =====
function $(id) { return document.getElementById(id); }

function pad(n, w) { return String(n).padStart(w, '0'); }

function timestamp() {
  const d = new Date();
  return pad(d.getHours(), 2) + ':' + pad(d.getMinutes(), 2) + ':' +
         pad(d.getSeconds(), 2) + '.' + pad(d.getMilliseconds(), 3);
}

function appendLog(area, text, className) {
  const div = document.createElement('div');
  div.className = 'log-line' + (className ? ' ' + className : '');
  div.textContent = '[' + timestamp() + '] ' + text;
  area.appendChild(div);
  // 限制 5000 行避免内存溢出
  while (area.children.length > 5000) {
    area.removeChild(area.firstChild);
  }
  area.scrollTop = area.scrollHeight;
}

// Base64 解码为字符串（UTF-8 安全）
function b64ToString(b64) {
  try {
    const bin = atob(b64);
    // 处理 UTF-8 多字节
    const bytes = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
    return new TextDecoder('utf-8').decode(bytes);
  } catch (e) {
    return '';
  }
}

// ===== 状态更新 =====
function setStateBadge(state) {
  const badge = $('state-badge');
  badge.className = 'badge';
  if (state === 'idle') {
    badge.classList.add('idle');
    badge.textContent = '空闲';
  } else if (state === 'burning') {
    badge.classList.add('burning');
    badge.textContent = '烧录中';
  } else if (state === 'monitoring') {
    badge.classList.add('monitor');
    badge.textContent = '监控中';
  }
}

// ===== SSE 连接（端口 81）=====
function connectSSE() {
  if (es && es.readyState !== EventSource.CLOSED) {
    return;
  }
  // SSE 走 81 端口，与 HTTP 80 分开
  const url = 'http://' + location.hostname + ':81/';
  es = new EventSource(url);

  es.onopen = () => {
    $('ws-state').textContent = '已连接';
    if (esReconnectTimer) {
      clearTimeout(esReconnectTimer);
      esReconnectTimer = null;
    }
  };

  es.onmessage = (event) => {
    let msg;
    try { msg = JSON.parse(event.data); } catch (e) { return; }
    switch (msg.type) {
      case 'burn_progress':
        updateBurnProgress(msg);
        break;
      case 'monitor_start':
        onMonitorStart(msg.baud);
        break;
      case 'monitor_data':
        if (!monitorPaused) {
          appendMonitorData(b64ToString(msg.data));
        }
        break;
      case 'monitor_stop':
        onMonitorStop();
        break;
      case 'monitor_reset':
        onMonitorReset();
        break;
      case 'wifi_connected':
        if (msg.ip) {
          $('wifi-connect-status').innerHTML =
            '<strong style="color: var(--success)">连接成功！</strong> 新 IP: ' + msg.ip +
            '<br>请切换到 ' + msg.ssid + ' WiFi 后访问 http://' + msg.ip + '/';
        }
        break;
      case 'wifi_connect_failed':
        $('wifi-connect-status').innerHTML =
          '<strong style="color: var(--danger)">连接失败</strong>：' + (msg.ssid || '') +
          ' 密码错误或信号太弱，ESP8266 已切回 AP 模式';
        $('wifi-connect-btn').disabled = false;
        break;
      case 'status':
        if (msg.state) setStateBadge(msg.state);
        break;
    }
  };

  es.onerror = () => {
    $('ws-state').textContent = '已断开';
    // EventSource 会自动重连，但 ESP8266 断电后需手动兜底
    if (es.readyState === EventSource.CLOSED) {
      if (esReconnectTimer) clearTimeout(esReconnectTimer);
      esReconnectTimer = setTimeout(connectSSE, 2000);
    }
  };
}

// ===== 标签切换 =====
document.querySelectorAll('.tab-btn').forEach(btn => {
  btn.addEventListener('click', () => {
    document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
    document.querySelectorAll('.tab-content').forEach(c => c.classList.remove('active'));
    btn.classList.add('active');
    $('tab-' + btn.dataset.tab).classList.add('active');
  });
});

// ===== hex2bin：Intel HEX 转 BIN（参考 diyruz_rt/Tools/hex2bin.py）=====
// 拆分为 parseHexToMap + mapToBin 两个函数，便于 OTA 分体固件合并复用

// 解析 HEX 文件到共享 dataMap
// - dataMap: Map<addr, Uint8Array>，可跨多个 hex 累积
// - addrRange: { min, max, countType0, countType2, countType4 }，函数内更新
// - log: 字符串数组，函数内追加
// 抛出异常时 dataMap/addrRange 可能已部分更新（调用方应丢弃）
async function parseHexToMap(file, dataMap, addrRange, log) {
  log.push('HEX 文件: ' + file.name);

  const text = await file.text();
  const lines = text.split(/\r?\n/);
  log.push('总行数: ' + lines.length);

  let baseAddr = 0x00000;

  for (let lineNo = 0; lineNo < lines.length; lineNo++) {
    const line = lines[lineNo].trim();
    if (!line || line[0] !== ':') continue;

    // 解析 hex 字节
    const raw = [];
    for (let i = 1; i + 1 < line.length; i += 2) {
      const b = parseInt(line.substr(i, 2), 16);
      if (isNaN(b)) throw new Error(file.name + ' line ' + (lineNo + 1) + ': 非法 hex 字符');
      raw.push(b);
    }
    if (raw.length < 5) continue;

    const bCount = raw[0];
    const bAddr = (raw[1] << 8) | raw[2];
    const bType = raw[3];
    const data = raw.slice(4, 4 + bCount);

    // 校验和验证
    let sum = 0;
    for (let i = 0; i < raw.length - 1; i++) sum = (sum + raw[i]) & 0xFF;
    const csum = raw[raw.length - 1];
    if (((sum + csum) & 0xFF) !== 0) {
      throw new Error(file.name + ' line ' + (lineNo + 1) + ': 校验和错误');
    }

    if (bType === 0x01) {
      // 结束记录
      break;
    } else if (bType === 0x02) {
      // 扩展段地址
      baseAddr = ((data[0] << 8) | data[1]) << 4;
      addrRange.countType2++;
    } else if (bType === 0x04) {
      // 扩展线性地址
      baseAddr = ((data[0] << 8) | data[1]) << 16;
      addrRange.countType4++;
    } else if (bType === 0x05) {
      // 起始线性地址，忽略
    } else if (bType === 0x00) {
      // 数据记录
      const physAddr = baseAddr + bAddr;
      dataMap.set(physAddr, new Uint8Array(data));
      if (addrRange.min === null || physAddr < addrRange.min) addrRange.min = physAddr;
      const endAddr = physAddr + data.length - 1;
      if (addrRange.max === null || endAddr > addrRange.max) addrRange.max = endAddr;
      addrRange.countType0++;
    } else {
      log.push('警告 ' + file.name + ' line ' + (lineNo + 1) + ': 未知记录类型 0x' + bType.toString(16));
    }
  }
}

// dataMap 转 256KB BIN（CC2530 要求完整 256KB）
// 返回 { bin, binSize, padBytes }
function mapToBin(dataMap, maxAddr) {
  const padTo = 0x40000;  // 256KB
  const binSize = Math.max(maxAddr + 1, padTo);
  const padBytes = binSize - (maxAddr + 1);

  const bin = new Uint8Array(binSize);
  bin.fill(0xFF);
  for (const [addr, data] of dataMap) {
    bin.set(data, addr);
  }
  return { bin: bin, binSize: binSize, padBytes: padBytes };
}

// 单文件 hex2bin（原有接口，行为不变）
// 返回 { bin: Uint8Array, log: [strings], name: string }
async function hex2bin(file) {
  const log = [];
  const dataMap = new Map();
  const addrRange = { min: null, max: null, countType0: 0, countType2: 0, countType4: 0 };

  await parseHexToMap(file, dataMap, addrRange, log);

  if (addrRange.min === null) throw new Error('HEX 文件无数据记录');

  log.push('数据记录 type00: ' + addrRange.countType0);
  log.push('扩展线性地址 type04: ' + addrRange.countType4);
  log.push('扩展段地址 type02: ' + addrRange.countType2);
  log.push('地址范围: 0x' + addrRange.min.toString(16) + ' - 0x' + addrRange.max.toString(16));
  const dataSpan = addrRange.max - addrRange.min + 1;
  log.push('数据跨度: ' + dataSpan + ' 字节 (' + (dataSpan / 1024).toFixed(1) + ' KB)');

  const { bin, binSize, padBytes } = mapToBin(dataMap, addrRange.max);
  log.push('BIN 大小: ' + binSize + ' 字节 (' + (binSize / 1024).toFixed(1) + ' KB)');
  log.push('尾部填充 0xFF: ' + padBytes + ' 字节 (' + (padBytes / 1024).toFixed(1) + ' KB)');

  const baseName = file.name.replace(/\.hex$/i, '');
  return { bin: bin, log: log, name: baseName + '.bin' };
}

// ===== OTA 分体固件合并 =====
// 解析 Boot.hex + App.hex 到同一个 dataMap，生成合并 BIN
// 地址冲突检测：Bootloader 应在 0x0000~0x07FF，应用在 0x0800+，重叠则报错
// 返回 { bin, log, name: 'OTA_merged.bin', bootRange, appRange }
async function mergeOtaHex(bootFile, appFile) {
  const log = [];
  const dataMap = new Map();
  const addrRange = { min: null, max: null, countType0: 0, countType2: 0, countType4: 0 };

  // 解析 Bootloader（先单独解析一次，记录地址范围）
  log.push('=== OTA Bootloader ===');
  const bootRange = { min: null, max: null };
  {
    const tmpMap = new Map();
    const tmpRange = { min: null, max: null, countType0: 0, countType2: 0, countType4: 0 };
    await parseHexToMap(bootFile, tmpMap, tmpRange, log);
    if (tmpRange.min === null) throw new Error('Bootloader HEX 无数据记录');
    bootRange.min = tmpRange.min;
    bootRange.max = tmpRange.max;
    // 合并到主 dataMap
    for (const [a, d] of tmpMap) dataMap.set(a, d);
    if (addrRange.min === null || tmpRange.min < addrRange.min) addrRange.min = tmpRange.min;
    if (addrRange.max === null || tmpRange.max > addrRange.max) addrRange.max = tmpRange.max;
    addrRange.countType0 += tmpRange.countType0;
    addrRange.countType2 += tmpRange.countType2;
    addrRange.countType4 += tmpRange.countType4;
  }

  // 解析应用固件
  log.push('=== 应用固件 ===');
  const appRange = { min: null, max: null };
  {
    const tmpMap = new Map();
    const tmpRange = { min: null, max: null, countType0: 0, countType2: 0, countType4: 0 };
    await parseHexToMap(appFile, tmpMap, tmpRange, log);
    if (tmpRange.min === null) throw new Error('应用固件 HEX 无数据记录');
    appRange.min = tmpRange.min;
    appRange.max = tmpRange.max;
    // 地址冲突检测：Bootloader 和应用不应重叠
    // Bootloader 在 0x0000~0x07FF，应用在 0x0800+，正常不重叠
    if (tmpRange.min <= bootRange.max) {
      throw new Error('地址冲突：应用固件起始 0x' + tmpRange.min.toString(16) +
                      ' <= Bootloader 结束 0x' + bootRange.max.toString(16) +
                      '，期望 Bootloader 在 0x0000~0x07FF，应用在 0x0800+');
    }
    // 合并到主 dataMap（地址不重叠，直接 set）
    for (const [a, d] of tmpMap) dataMap.set(a, d);
    if (tmpRange.max > addrRange.max) addrRange.max = tmpRange.max;
    addrRange.countType0 += tmpRange.countType0;
    addrRange.countType2 += tmpRange.countType2;
    addrRange.countType4 += tmpRange.countType4;
  }

  log.push('=== 合并结果 ===');
  log.push('Bootloader: 0x' + bootRange.min.toString(16) + ' ~ 0x' + bootRange.max.toString(16) +
           ' (' + (bootRange.max - bootRange.min + 1) + ' 字节)');
  log.push('应用固件:   0x' + appRange.min.toString(16) + ' ~ 0x' + appRange.max.toString(16) +
           ' (' + (appRange.max - appRange.min + 1) + ' 字节)');
  log.push('合并后地址范围: 0x' + addrRange.min.toString(16) + ' ~ 0x' + addrRange.max.toString(16));
  log.push('数据记录总数: ' + addrRange.countType0);

  const { bin, binSize, padBytes } = mapToBin(dataMap, addrRange.max);
  log.push('合并 BIN: ' + binSize + ' 字节 (' + (binSize / 1024).toFixed(1) + ' KB)');

  return {
    bin: bin,
    log: log,
    name: 'OTA_merged.bin',
    bootRange: bootRange,
    appRange: appRange
  };
}

// 轻量解析：只算地址范围，不构造 dataMap（用于文件选择时即时预览）
async function quickHexInfo(file) {
  const tmpMap = new Map();
  const tmpRange = { min: null, max: null, countType0: 0, countType2: 0, countType4: 0 };
  await parseHexToMap(file, tmpMap, tmpRange, []);
  if (tmpRange.min === null) throw new Error(file.name + ': 无数据记录');
  return {
    min: tmpRange.min,
    max: tmpRange.max,
    span: tmpRange.max - tmpRange.min + 1
  };
}

// ===== 烧录模式切换 =====
document.querySelectorAll('input[name="burn-mode"]').forEach(r => {
  r.addEventListener('change', e => {
    document.getElementById('single-mode').style.display =
      e.target.value === 'single' ? 'block' : 'none';
    document.getElementById('ota-mode').style.display =
      e.target.value === 'ota' ? 'block' : 'none';
  });
});

// OTA 文件选择：两个文件都选好后即时预览地址范围
async function updateOtaPreview() {
  const bootFile = $('ota-boot-file').files[0];
  const appFile = $('ota-app-file').files[0];
  const btn = $('ota-merge-upload-btn');
  const preview = $('ota-merge-preview');
  const bootInfo = $('ota-boot-info');
  const appInfo = $('ota-app-info');

  bootInfo.textContent = '';
  appInfo.textContent = '';
  preview.textContent = '';
  preview.classList.remove('error');
  btn.disabled = true;

  if (bootFile) {
    try {
      const info = await quickHexInfo(bootFile);
      bootInfo.textContent = '0x' + info.min.toString(16) + ' ~ 0x' + info.max.toString(16) +
                            ' (' + (info.span / 1024).toFixed(1) + ' KB)';
    } catch (e) {
      bootInfo.textContent = '解析失败: ' + e.message;
    }
  }
  if (appFile) {
    try {
      const info = await quickHexInfo(appFile);
      appInfo.textContent = '0x' + info.min.toString(16) + ' ~ 0x' + info.max.toString(16) +
                           ' (' + (info.span / 1024).toFixed(1) + ' KB)';
    } catch (e) {
      appInfo.textContent = '解析失败: ' + e.message;
    }
  }

  if (!bootFile || !appFile) return;

  // 完整预览
  try {
    const bootInfo2 = await quickHexInfo(bootFile);
    const appInfo2 = await quickHexInfo(appFile);
    const overlap = appInfo2.min <= bootInfo2.max;
    const bootClass = (bootInfo2.min === 0 && bootInfo2.max <= 0x07FF) ? 'addr-ok' : 'addr-warn';
    const appClass = (appInfo2.min >= 0x0800) ? 'addr-ok' : 'addr-warn';
    let html = 'Bootloader  0x' + bootInfo2.min.toString(16).padStart(4, '0') +
               ' ~ 0x' + bootInfo2.max.toString(16).padStart(4, '0') +
               '  (' + (bootInfo2.span / 1024).toFixed(1) + ' KB)\n';
    html += '应用固件    0x' + appInfo2.min.toString(16).padStart(4, '0') +
            ' ~ 0x' + appInfo2.max.toString(16).padStart(4, '0') +
            '  (' + (appInfo2.span / 1024).toFixed(1) + ' KB)\n';
    html += '合并后 BIN  256 KB';
    if (overlap) {
      html += '\n⚠️ 地址重叠：应用起始 0x' + appInfo2.min.toString(16) +
              ' <= Bootloader 结束 0x' + bootInfo2.max.toString(16);
      preview.classList.add('error');
      btn.disabled = true;
    } else {
      btn.disabled = false;
    }
    preview.textContent = html;
  } catch (e) {
    preview.textContent = '预览失败: ' + e.message;
    preview.classList.add('error');
  }
}

$('ota-boot-file').addEventListener('change', updateOtaPreview);
$('ota-app-file').addEventListener('change', updateOtaPreview);

// 合并并上传
$('ota-merge-upload-btn').addEventListener('click', async () => {
  const bootFile = $('ota-boot-file').files[0];
  const appFile = $('ota-app-file').files[0];
  if (!bootFile || !appFile) {
    alert('请选择两个 .hex 文件');
    return;
  }
  const btn = $('ota-merge-upload-btn');
  const progress = $('ota-merge-progress');
  btn.disabled = true;
  progress.textContent = '合并中...';

  try {
    const result = await mergeOtaHex(bootFile, appFile);
    progress.innerHTML = result.log.join('<br>') + '<br>上传中...';

    const blob = new Blob([result.bin], { type: 'application/octet-stream' });
    const formData = new FormData();
    formData.append('file', blob, result.name);

    const resp = await fetch('/api/upload', { method: 'POST', body: formData });
    const data = await resp.json();
    if (data.success) {
      progress.innerHTML = result.log.join('<br>') +
                           '<br><strong style="color: var(--success)">上传成功: ' +
                           data.filename + ' (' + data.size + ' 字节)</strong>';
      refreshFileList();
      // 自动选中新上传的合并 BIN
      selectedFile = data.filename;
      $('selected-file').textContent = selectedFile;
      setTimeout(refreshFileList, 100);  // 列表刷新后再标 selected
    } else {
      progress.innerHTML = result.log.join('<br>') +
                           '<br><strong style="color: var(--danger)">上传失败: ' +
                           (data.error || '未知错误') + '</strong>';
    }
  } catch (e) {
    progress.innerHTML = '<strong style="color: var(--danger)">合并失败: ' + e.message + '</strong>';
  } finally {
    btn.disabled = false;
  }
});

// ===== 文件上传 =====
$('upload-btn').addEventListener('click', async () => {
  const input = $('file-input');
  if (!input.files || input.files.length === 0) {
    alert('请先选择文件');
    return;
  }
  const file = input.files[0];
  const isHex = /\.hex$/i.test(file.name);

  $('upload-btn').disabled = true;

  try {
    let uploadFile;
    let uploadName;
    if (isHex) {
      $('upload-progress').textContent = 'HEX → BIN 转换中...';
      try {
        const result = await hex2bin(file);
        uploadFile = new Blob([result.bin], { type: 'application/octet-stream' });
        uploadName = result.name;
        // 显示转换日志
        $('upload-progress').innerHTML = result.log.join('<br>');
      } catch (e) {
        $('upload-progress').textContent = 'HEX 转换失败: ' + e.message;
        return;
      }
    } else {
      uploadFile = file;
      uploadName = file.name;
      $('upload-progress').textContent = '上传中...';
    }

    const formData = new FormData();
    formData.append('file', uploadFile, uploadName);

    if (!isHex) $('upload-progress').textContent = '上传中...';
    else $('upload-progress').innerHTML += '<br>上传中...';

    const resp = await fetch('/api/upload', { method: 'POST', body: formData });
    const result = await resp.json();
    if (result.success) {
      const html = isHex ? $('upload-progress').innerHTML + '<br>' : '';
      $('upload-progress').innerHTML = html + '上传成功: ' + result.filename + ' (' + result.size + ' 字节)';
      refreshFileList();
    } else {
      $('upload-progress').textContent = '上传失败: ' + (result.error || '未知错误');
    }
  } catch (e) {
    $('upload-progress').textContent = '上传失败: ' + e.message;
  } finally {
    $('upload-btn').disabled = false;
    input.value = '';
  }
});

// ===== 文件列表 =====
async function refreshFileList() {
  try {
    const resp = await fetch('/api/files');
    const result = await resp.json();
    if (!result.success) return;
    const list = $('file-list');
    list.innerHTML = '';
    if (result.files.length === 0) {
      list.innerHTML = '<div style="color: var(--text-muted)">暂无固件，请上传</div>';
      return;
    }
    result.files.forEach(f => {
      const item = document.createElement('div');
      item.className = 'file-item';
      if (f.name === selectedFile) item.classList.add('selected');
      const sizeKB = (f.size / 1024).toFixed(1);
      const date = new Date(f.time * 1000).toLocaleString('zh-CN');

      const info = document.createElement('div');
      info.className = 'file-info';
      const nameSpan = document.createElement('span');
      nameSpan.className = 'file-name';
      nameSpan.textContent = f.name;  // textContent 防 XSS
      const sizeSpan = document.createElement('span');
      sizeSpan.className = 'file-meta';
      sizeSpan.textContent = sizeKB + ' KB';
      const dateSpan = document.createElement('span');
      dateSpan.className = 'file-meta';
      dateSpan.textContent = date;
      info.appendChild(nameSpan);
      info.appendChild(sizeSpan);
      info.appendChild(dateSpan);

      const delBtn = document.createElement('button');
      delBtn.className = 'btn danger delete-btn';
      delBtn.dataset.name = f.name;  // dataset 自动转义
      delBtn.textContent = '删除';

      item.appendChild(info);
      item.appendChild(delBtn);
      list.appendChild(item);
    });
    // 选中事件
    list.querySelectorAll('.file-name').forEach(el => {
      el.addEventListener('click', () => {
        selectedFile = el.textContent;
        $('selected-file').textContent = selectedFile;
        list.querySelectorAll('.file-item').forEach(i => i.classList.remove('selected'));
        el.closest('.file-item').classList.add('selected');
      });
    });
    // 删除事件
    list.querySelectorAll('.delete-btn').forEach(btn => {
      btn.addEventListener('click', async () => {
        if (!confirm('删除 ' + btn.dataset.name + '?')) return;
        const resp = await fetch('/api/files/' + encodeURIComponent(btn.dataset.name), { method: 'DELETE' });
        const r = await resp.json();
        if (r.success) {
          if (selectedFile === btn.dataset.name) {
            selectedFile = '';
            $('selected-file').textContent = '未选择';
          }
          refreshFileList();
        } else {
          alert('删除失败: ' + r.error);
        }
      });
    });
  } catch (e) {
    console.error('refreshFileList error:', e);
  }
}

// ===== 烧录 =====
$('burn-btn').addEventListener('click', async () => {
  if (!selectedFile) {
    alert('请先选择固件');
    return;
  }
  const verify = $('verify-check').checked;
  $('burn-btn').disabled = true;
  $('burn-log').innerHTML = '';
  $('burn-progress-bar').style.width = '0%';
  $('burn-progress-text').textContent = '0%';
  appendLog($('burn-log'), '开始烧录: ' + selectedFile + (verify ? ' (校验)' : ''), 'success');

  try {
    const resp = await fetch('/api/burn', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ filename: selectedFile, verify: verify })
    });
    const result = await resp.json();
    if (!result.success) {
      appendLog($('burn-log'), '启动失败: ' + result.error, 'error');
      $('burn-btn').disabled = false;
    } else {
      appendLog($('burn-log'), '总块数: ' + result.total_blocks);
    }
  } catch (e) {
    appendLog($('burn-log'), '请求失败: ' + e.message, 'error');
    $('burn-btn').disabled = false;
  }
});

function updateBurnProgress(msg) {
  $('burn-progress-bar').style.width = msg.percent + '%';
  $('burn-progress-text').textContent = msg.percent + '% (' + msg.current_block + '/' + msg.total_blocks + ')';
  if (msg.current_block > 0 && msg.current_block % 50 === 0 && msg.current_block < msg.total_blocks) {
    appendLog($('burn-log'), '写入块 ' + msg.current_block + '/' + msg.total_blocks);
  }
  if (msg.error) {
    appendLog($('burn-log'), '错误: ' + msg.error, 'error');
  }
  if (msg.done) {
    appendLog($('burn-log'), '烧录完成', 'success');
    $('burn-btn').disabled = false;
    // 自动切换到监控页
    setTimeout(() => {
      document.querySelector('.tab-btn[data-tab="monitor"]').click();
    }, 1000);
  }
}

// ===== 监控 =====
$('monitor-start-btn').addEventListener('click', async () => {
  const baud = parseInt($('baud-select').value);
  const autoReset = $('auto-reset-check').checked;
  $('monitor-start-btn').disabled = true;
  try {
    const resp = await fetch('/api/monitor', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ baud: baud, auto_reset: autoReset })
    });
    const result = await resp.json();
    if (!result.success) {
      alert('启动监控失败: ' + result.error);
      $('monitor-start-btn').disabled = false;
    }
    // 等待 monitor_start SSE 事件再切按钮状态
  } catch (e) {
    alert('请求失败: ' + e.message);
    $('monitor-start-btn').disabled = false;
  }
});

// 手动复位 CC2530（监控中也可用，复位后从 main() 重新输出日志）
$('reset-cc-btn').addEventListener('click', async () => {
  $('reset-cc-btn').disabled = true;
  try {
    const resp = await fetch('/api/reset', { method: 'POST' });
    const result = await resp.json();
    if (result.success) {
      // 监控中复位会收到 monitor_reset 事件清空日志
      // 非监控中只复位硬件
    } else {
      alert('复位失败: ' + (result.error || '未知错误'));
    }
  } catch (e) {
    alert('请求失败: ' + e.message);
  } finally {
    $('reset-cc-btn').disabled = false;
  }
});

$('monitor-stop-btn').addEventListener('click', async () => {
  try {
    await fetch('/api/stop', { method: 'POST' });
  } catch (e) {}
});

function onMonitorStart(baud) {
  monitorActive = true;
  monitorBytes = 0;
  monitorBuffer = '';
  $('bytes-received').textContent = '0';
  $('monitor-log').innerHTML = '';
  $('monitor-state').textContent = '监控中 @ ' + baud + ' bps';
  $('monitor-start-btn').disabled = true;
  $('monitor-stop-btn').disabled = false;
  $('reset-cc-btn').disabled = false;
  $('pause-btn').disabled = false;
  $('clear-btn').disabled = false;
  $('download-btn').disabled = false;
  $('search-input').disabled = false;
  const autoReset = $('auto-reset-check').checked;
  if (autoReset) {
    appendLog($('monitor-log'), '监控开始 @ ' + baud + ' bps（已自动复位 CC2530）', 'success');
  } else {
    appendLog($('monitor-log'), '监控开始 @ ' + baud + ' bps，点"复位 CC2530"看启动日志', 'success');
  }
}

function onMonitorStop() {
  monitorActive = false;
  $('monitor-state').textContent = '已停止';
  $('monitor-start-btn').disabled = false;
  $('monitor-stop-btn').disabled = true;
  $('reset-cc-btn').disabled = true;
  $('pause-btn').disabled = true;
  $('pause-btn').textContent = '暂停';
  appendLog($('monitor-log'), '监控已停止', 'error');
}

// 监控中收到 monitor_reset 事件：CC2530 已复位，清空日志区准备接收启动日志
function onMonitorReset() {
  monitorBytes = 0;
  monitorBuffer = '';
  $('bytes-received').textContent = '0';
  $('monitor-log').innerHTML = '';
  appendLog($('monitor-log'), 'CC2530 已复位，等待启动日志...', 'success');
}

function appendMonitorData(data) {
  monitorBytes += data.length;
  $('bytes-received').textContent = monitorBytes;
  monitorBuffer += data;
  const log = $('monitor-log');
  // 按行分割
  let idx;
  while ((idx = monitorBuffer.indexOf('\n')) >= 0) {
    let line = monitorBuffer.substring(0, idx + 1);
    monitorBuffer = monitorBuffer.substring(idx + 1);
    // 搜索过滤
    const filter = $('search-input').value.trim();
    if (filter && line.indexOf(filter) < 0) continue;
    const div = document.createElement('div');
    div.className = 'log-line';
    div.textContent = '[' + timestamp() + '] ' + line.replace(/\r?\n$/, '');
    log.appendChild(div);
    while (log.children.length > 5000) log.removeChild(log.firstChild);
  }
  log.scrollTop = log.scrollHeight;
}

$('pause-btn').addEventListener('click', () => {
  monitorPaused = !monitorPaused;
  $('pause-btn').textContent = monitorPaused ? '继续' : '暂停';
});

$('clear-btn').addEventListener('click', () => {
  $('monitor-log').innerHTML = '';
  monitorBuffer = '';
});

$('download-btn').addEventListener('click', () => {
  const lines = [];
  $('monitor-log').querySelectorAll('.log-line').forEach(el => lines.push(el.textContent));
  const blob = new Blob([lines.join('\n')], { type: 'text/plain' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = 'cc2530_monitor_' + Date.now() + '.log';
  a.click();
  URL.revokeObjectURL(a.href);
});

$('search-input').addEventListener('input', () => {
  // 实时过滤由 appendMonitorData 处理；已显示的不重过滤（简化）
});

// ===== 设置 =====
async function loadConfig() {
  try {
    const resp = await fetch('/api/config');
    const cfg = await resp.json();
    $('wifi-ssid').value = cfg.wifi_ssid || '';
    $('wifi-password').value = cfg.wifi_password || '';
    $('default-baud-select').value = cfg.monitor_baud || 115200;
    $('default-verify-check').checked = !!cfg.verify;
    $('baud-select').value = cfg.monitor_baud || 115200;
    $('verify-check').checked = !!cfg.verify;
  } catch (e) {
    console.error('loadConfig error:', e);
  }
}

// ===== WiFi 配网 =====
function rssiClass(rssi) {
  if (rssi >= -55) return 'strong';
  if (rssi >= -75) return 'medium';
  return 'weak';
}

function rssiLabel(rssi) {
  if (rssi >= -55) return '强';
  if (rssi >= -75) return '中';
  return '弱';
}

async function scanWifi() {
  $('wifi-scan-btn').disabled = true;
  $('wifi-scan-status').textContent = '扫描中...';
  $('wifi-list').innerHTML = '';
  try {
    const resp = await fetch('/api/wifi/scan');
    const result = await resp.json();
    if (!result.success) {
      $('wifi-scan-status').textContent = '扫描失败: ' + (result.error || '未知错误');
      return;
    }
    const networks = result.networks || [];
    $('wifi-scan-status').textContent = '找到 ' + networks.length + ' 个网络';
    if (networks.length === 0) {
      $('wifi-list').innerHTML = '<div class="hint">未找到网络</div>';
      return;
    }
    const list = $('wifi-list');
    networks.forEach(n => {
      const item = document.createElement('div');
      item.className = 'wifi-item';
      const left = document.createElement('span');
      left.className = 'wifi-ssid';
      left.textContent = n.ssid;
      if (n.encrypted) {
        const lock = document.createElement('span');
        lock.className = 'wifi-lock';
        lock.textContent = '🔒';
        left.appendChild(lock);
      }
      const right = document.createElement('span');
      right.className = 'wifi-meta';
      const rssi = document.createElement('span');
      rssi.className = 'wifi-rssi ' + rssiClass(n.rssi);
      rssi.textContent = rssiLabel(n.rssi) + ' (' + n.rssi + 'dBm)';
      right.appendChild(rssi);
      item.appendChild(left);
      item.appendChild(right);
      item.addEventListener('click', () => {
        $('wifi-ssid').value = n.ssid;
        $('wifi-password').value = '';
        $('wifi-password').focus();
        list.querySelectorAll('.wifi-item').forEach(i => i.classList.remove('selected'));
        item.classList.add('selected');
      });
      list.appendChild(item);
    });
  } catch (e) {
    $('wifi-scan-status').textContent = '扫描失败: ' + e.message;
  } finally {
    $('wifi-scan-btn').disabled = false;
  }
}

$('wifi-scan-btn').addEventListener('click', scanWifi);

$('wifi-connect-btn').addEventListener('click', async () => {
  const ssid = $('wifi-ssid').value.trim();
  const pwd = $('wifi-password').value;
  if (!ssid) {
    alert('请输入或选择 SSID');
    return;
  }
  $('wifi-connect-btn').disabled = true;
  $('wifi-connect-status').textContent = '连接中...（ESP8266 切换到 STA 模式，连接成功后请切换到新 WiFi 访问）';
  try {
    const resp = await fetch('/api/wifi/connect', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ssid: ssid, password: pwd })
    });
    const result = await resp.json();
    if (result.success) {
      // 响应只是"开始连接"，真正的结果通过 SSE 或下一次状态查询得知
      $('wifi-connect-status').textContent = '正在连接，等待结果...';
      // 等 10 秒后查询状态（如果 ESP8266 已切换 STA，AP 会断开，请求会失败）
      setTimeout(async () => {
        try {
          const sr = await fetch('/api/status');
          const s = await sr.json();
          if (s.wifi && s.wifi.mode === 'sta' && s.wifi.ip && s.wifi.ip !== '0.0.0.0') {
            $('wifi-connect-status').innerHTML =
              '<strong style="color: var(--success)">连接成功！</strong><br>' +
              '新 IP: ' + s.wifi.ip + '<br>' +
              '请切换到 ' + ssid + ' WiFi 后访问 http://' + s.wifi.ip + '/';
          } else {
            $('wifi-connect-status').textContent = '连接失败，请检查密码或信号';
          }
        } catch (e) {
          // ESP8266 已切换 STA，AP 断开，无法访问
          $('wifi-connect-status').innerHTML =
            'ESP8266 已切换网络模式。请将电脑/手机切回 <strong>' + ssid +
            '</strong> WiFi，然后通过串口监视器查看新 IP，或访问路由器后台查找。';
        }
      }, 10000);
    } else {
      $('wifi-connect-status').textContent = '启动失败: ' + (result.error || '未知错误');
      $('wifi-connect-btn').disabled = false;
    }
  } catch (e) {
    $('wifi-connect-status').textContent = '请求失败: ' + e.message;
    $('wifi-connect-btn').disabled = false;
  }
});

$('save-monitor-btn').addEventListener('click', async () => {
  const cfg = { monitor_baud: parseInt($('default-baud-select').value) };
  const resp = await fetch('/api/config', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(cfg)
  });
  const r = await resp.json();
  if (r.success) {
    $('baud-select').value = cfg.monitor_baud;
    alert('已保存');
  } else {
    alert('保存失败: ' + r.error);
  }
});

$('save-burn-btn').addEventListener('click', async () => {
  const cfg = { verify: $('default-verify-check').checked };
  const resp = await fetch('/api/config', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(cfg)
  });
  const r = await resp.json();
  if (r.success) {
    $('verify-check').checked = cfg.verify;
    alert('已保存');
  } else {
    alert('保存失败: ' + r.error);
  }
});

$('reboot-btn').addEventListener('click', async () => {
  if (!confirm('确定重启 ESP8266?')) return;
  try {
    await fetch('/api/reboot', { method: 'POST' });
    alert('已发送重启指令，等待重新连接...');
  } catch (e) {
    alert('请求失败: ' + e.message);
  }
});

// ===== 状态轮询 =====
async function pollStatus() {
  try {
    const resp = await fetch('/api/status');
    const s = await resp.json();
    setStateBadge(s.state);
    if (s.wifi) {
      $('ip-info').textContent = 'IP: ' + s.wifi.ip;
      $('device-ip').textContent = s.wifi.ip;
      $('rssi').textContent = s.wifi.rssi;
    }
    // 配网模式提示
    if (s.config_mode !== undefined) {
      const hint = $('wifi-mode-hint');
      if (s.config_mode) {
        hint.innerHTML = '<strong style="color: var(--warning)">配网模式</strong>：ESP8266 开放 AP "CCLoader-Setup"，请保持电脑/手机连此 AP 完成配网';
      } else if (s.wifi && s.wifi.mode === 'sta') {
        hint.innerHTML = '<strong style="color: var(--success)">STA 模式</strong>：已连接 ' + (s.wifi.ssid || '') + '，IP ' + (s.wifi.ip || '-');
      } else {
        hint.textContent = '';
      }
    }
    if (s.uptime !== undefined) {
      const h = Math.floor(s.uptime / 3600);
      const m = Math.floor((s.uptime % 3600) / 60);
      $('uptime').textContent = h + '时' + m + '分';
    }
    if (s.monitor) {
      if (s.monitor.active && !monitorActive) {
        onMonitorStart(s.monitor.baud);
      } else if (!s.monitor.active && monitorActive) {
        onMonitorStop();
      }
    }
  } catch (e) {}
}

// ===== 初始化 =====
function init() {
  connectSSE();
  refreshFileList();
  loadConfig();
  pollStatus();
  setInterval(pollStatus, 3000);
}

init();

)=====";
const size_t app_js_len = 36728;

// config.json (96 bytes, application/json)
const char config_json[] PROGMEM = R"=====(
{
  "wifi_ssid": "",
  "wifi_password": "",
  "monitor_baud": 115200,
  "verify": false
}

)=====";
const size_t config_json_len = 96;

}  // namespace WebAssets
