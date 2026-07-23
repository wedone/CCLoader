# CCLoader WebUI 帮助

> NodeMCU ESP8266 + CC2530 烧录/监控一体机。本文档由 `/api/help` 返回，WebUI 帮助页与 AI Agent 共用同一份内容。
> 固件版本: CCLoader-WebUI v1.1

---

## 1. 接线方法

NodeMCU ESP8266 与 CC2530 模块共需要 5 根线（含共地）：

| ESP8266 GPIO | NodeMCU 丝印 | 方向 | CC2530 引脚 | 用途 |
|---|---|---|---|---|
| GPIO5  | D1  | → | Pin 7 (RESETn)    | CC Debug 复位 |
| GPIO4  | D2  | → | Pin 3 (DC)        | CC Debug 时钟 |
| GPIO12 | D6  | ↔ | Pin 4 (DD)        | CC Debug 数据（双向） |
| GPIO3  | RX  | ← | P0_3 (UART0 TX)   | 监控串口日志 |
| GND    | GND | — | GND               | 共地（必须） |

CC2530 的 3.3V 可从 NodeMCU 的 3V3 引脚取，电流 < 50mA。

```
   NodeMCU ESP8266                     CC2530 模块
  ┌────────────────┐                ┌─────────────────┐
  │            3V3 ├───────────────►│ VCC             │
  │            GND │◄──────────────►│ GND             │
  │  D1 (GPIO5)    ├───────────────►│ Pin 7 RESETn    │
  │  D2 (GPIO4)    ├───────────────►│ Pin 3 DC        │
  │  D6 (GPIO12)   │◄──────────────►│ Pin 4 DD        │
  │  RX (GPIO3)    │◄───────────────│ P0_3 UART0 TX   │
  └────────────────┘                └─────────────────┘
```

---

## 2. API 用法（自动化调用）

无鉴权，局域网内任意设备可调用。完整流程：上传 BIN → 烧录 → 监控 → (可选) 重启。

### 2.1 关键规则

- **API 仅接受 `.bin`**：上传 `.hex` 会被拒绝并返回 400 + `hex_not_supported` 错误。浏览器端上传 `.hex` 会自动转换，API 调用需自行转换（算法见下方 2.3 节）。
- **烧录强制校验**：`/api/burn` 的 `verify` 参数被忽略，固件内部强制 `verify=true`，保证烧录正确性。烧录失败会通过 SSE `burn_progress.error` 和 `/api/status` 的 `burn.error` 字段报出。
- **烧录强制异步**：`/api/burn` 立即返回 `task_id`，烧录在后台执行。`?async=1` 参数兼容但非必需。通过轮询 `/api/status` 跟踪进度。
- **NTP 授时**：WiFi 连接后自动同步北京时间（UTC+8），`/api/status` 的 `time` 字段为当前 epoch 秒；未授时返回 0。

### 2.2 完整流程示例

```bash
#!/bin/bash
IP=10.0.0.147
BIN=DIYRuZRT_256k.bin

# 1. 上传 BIN（multipart/form-data，字段 file。仅 .bin）
curl -s -F "file=@${BIN}" http://${IP}/api/upload
# 返回: {"success":true,"filename":"DIYRuZRT_256k.bin","size":262144}

# 2. 发起烧录（强制异步 + 强制校验，verify 参数无效）
curl -s -X POST "http://${IP}/api/burn" \
  -H "Content-Type: application/json" \
  -d '{"filename":"DIYRuZRT_256k.bin"}'
# 返回: {"success":true,"async":true,"task_id":1,"total_blocks":512}

# 3. 轮询状态（每 2 秒，含 burn.progress / burn.error / time）
curl -s http://${IP}/api/status
# 返回: {"state":"burning","task_id":1,"burn_pending":false,
#        "burn":{"percent":50,"current_block":256,"total_blocks":512,"done":false,"error":""},
#        "time":1729000000}

# 4. 烧录完成（done=true 且 error 为空）后启动监控
curl -s -X POST http://${IP}/api/monitor \
  -H "Content-Type: application/json" \
  -d '{"baud":115200,"auto_reset":true}'

# 5. 轮询获取日志（断点续传，since 为上次返回的 total）
curl -s "http://${IP}/api/monitor/buffer?since=0"
# 返回: {"success":true,"total":16,"buffered":16,"offset":0,
#        "data":"<base64>","truncated":false}

# 6. 停止监控
curl -s -X POST http://${IP}/api/stop

# 7. 可选：重启烧录器（释放内存 / 重新初始化 CC2530）
curl -s -X POST http://${IP}/api/reboot
```

### 2.3 hex2bin 转换算法（API 调用必需）

API 仅接受 `.bin`，上传 `.hex` 会被拒绝。需在客户端自行转换。算法基于 Intel HEX 格式：

**记录格式**：每行以 `:` 开头，结构为 `:LLAAAATT[DD...]CC`
- `LL`：数据字节数
- `AAAA`：地址（在该记录类型上下文内）
- `TT`：记录类型（00=数据/01=结束/02=扩展段地址/04=扩展线性地址/05=起始地址）
- `DD...`：LL 字节数据
- `CC`：校验和，`(sum(除最后字节)) & 0xFF` 应为 0

**转换步骤**：
1. 维护 `baseAddr=0`，逐行解析
2. `TT=0x00` 数据记录：物理地址 = `baseAddr + AAAA`，写入 Map
3. `TT=0x04` 扩展线性地址：`baseAddr = data << 16`
4. `TT=0x02` 扩展段地址：`baseAddr = data << 4`
5. `TT=0x01` 结束记录：停止解析
6. `TT=0x05` 起始地址：忽略
7. 收集所有数据按物理地址排序，缺失地址填 `0xFF`
8. 尾部填充 `0xFF` 到 256KB（0x40000）以适配 CC2530F256

**Python 参考实现**：

```python
def hex2bin(hex_path, out_path=None, pad_to=0x40000):
    """Intel HEX 转 BIN，填充到 pad_to 字节（默认 256KB）"""
    data_map = {}      # phys_addr -> bytes
    base_addr = 0
    min_addr = None
    max_addr = None

    with open(hex_path, 'r') as f:
        for line_no, line in enumerate(f, 1):
            line = line.strip()
            if not line or line[0] != ':':
                continue
            raw = bytes.fromhex(line[1:])
            if len(raw) < 5:
                continue
            b_count = raw[0]
            b_addr = (raw[1] << 8) | raw[2]
            b_type = raw[3]
            data = raw[4:4 + b_count]
            # 校验和验证
            if (sum(raw[:-1]) & 0xFF) != 0:
                raise ValueError(f'line {line_no}: checksum error')
            if b_type == 0x01:       # 结束
                break
            elif b_type == 0x02:     # 扩展段地址
                base_addr = ((data[0] << 8) | data[1]) << 4
            elif b_type == 0x04:     # 扩展线性地址
                base_addr = ((data[0] << 8) | data[1]) << 16
            elif b_type == 0x05:     # 起始地址，忽略
                continue
            elif b_type == 0x00:     # 数据记录
                phys = base_addr + b_addr
                for i, b in enumerate(data):
                    data_map[phys + i] = b
                if min_addr is None or phys < min_addr:
                    min_addr = phys
                end = phys + len(data) - 1
                if max_addr is None or end > max_addr:
                    max_addr = end

    if min_addr is None:
        raise ValueError('HEX 无数据记录')

    # 构造 BIN，缺失地址填 0xFF
    bin_size = max(max_addr + 1, pad_to)
    bin_data = bytearray([0xFF] * bin_size)
    for addr, b in data_map.items():
        if addr < bin_size:
            bin_data[addr] = b

    if out_path:
        with open(out_path, 'wb') as f:
            f.write(bin_data)
    return bytes(bin_data)


# 使用示例
hex2bin('CC2530.hex', 'CC2530.bin')
# 然后 curl -F "file=@CC2530.bin" http://10.0.0.147/api/upload
```

**OTA 分体固件**（Bootloader + 应用）合并：分别解析两个 .hex 到同一个 `data_map`，Bootloader 应在 0x0000~0x07FF，应用在 0x0800+，地址不重叠则合并后统一填充到 256KB。

---

## 3. API 端点列表

| 方法 | 路径 | 功能 |
|---|---|---|
| GET  | /api/help | 返回本帮助文档（text/plain，markdown 格式） |
| GET  | /api/status | 当前状态（state/burn/monitor/wifi/task_id/time） |
| GET  | /api/config | 读取配置（ssid/baud/verify） |
| POST | /api/config | 更新配置（body: JSON） |
| GET  | /api/files | 列出已上传 BIN（含 size/time） |
| POST | /api/upload | 上传 BIN（multipart, 字段 file）。**.hex 会被拒绝**，返回 400 + hex2bin 提示 |
| DELETE | /api/files/{name} | 删除指定 BIN |
| POST | /api/burn | 烧录（强制异步 + 强制校验，立即返回 task_id） |
| POST | /api/monitor | 开始监控（body: {baud,auto_reset}） |
| GET  | /api/monitor/buffer?since=N | 获取日志（断点续传） |
| POST | /api/stop | 停止监控 |
| POST | /api/reset | 复位 CC2530（GPIO5/RESETn，监控中也可用） |
| POST | /api/reboot | 重启 ESP8266 |
| GET  | /api/wifi/scan | 扫描 WiFi |
| POST | /api/wifi/connect | 连接 WiFi（body: {ssid,password}） |

---

## 4. 实时事件（SSE 端口 81）

浏览器 EventSource 自动连接，或用 `curl -N` 监听：

```bash
curl -N http://10.0.0.147:81/
```

事件类型（`data:` 后为 JSON）：

- `{"type":"status","state":"idle|burning|monitoring"}`
- `{"type":"burn_progress","percent":50,"current_block":256,"total_blocks":512,"done":false,"error":""}`
- `{"type":"monitor_start","baud":115200}`
- `{"type":"monitor_data","data":"<base64>"}`
- `{"type":"monitor_reset"}` — CC2530 已复位（监控中），前端清空日志区
- `{"type":"monitor_stop"}`
- `{"type":"wifi_connected","ssid":"...","ip":"192.168.x.x"}`
- `{"type":"wifi_connect_failed","ssid":"..."}`

---

## 5. OTA 远程升级（免拔线）

首次 USB 烧录后，后续固件升级走 WiFi，永久免拔 TX 线。升级时 LittleFS 和 WiFi 配置都保留。

```bash
# 编译固件
python -m platformio run

# OTA 升级（10-30 秒自动重启）
curl -F "image=@.pio/build/nodemcuv2/firmware.bin" http://10.0.0.147/update
# 返回: Update Success! Rebooting...
```

修改 `data/` 目录下的页面文件后需重新生成内嵌资源：

```bash
python tools/gen_web_assets.py  # 重新生成 web_assets.h
python -m platformio run        # 重新编译
curl -F "image=@.pio/build/nodemcuv2/firmware.bin" http://10.0.0.147/update
```

升级期间 HTTP/SSE 暂不可用（约 10-30 秒），重启后自动恢复。

---

## 6. 首次使用流程

1. 按接线表接好 5 根线（CC2530 模块先不接 3V3 电源）
2. USB 接 NodeMCU 到电脑，首次烧录固件：
   ```
   pip install platformio
   python -m platformio run -t upload --upload-port COM5
   ```
3. 烧录 ESP8266 时 CC2530 的 TX 线（P0_3 → RX）要拔掉，避免干扰 ESP8266 下载
4. 烧完后插回 TX 线，给 CC2530 通电
5. 手机/电脑连 WiFi `CCLoader-Setup`（首次无密码）
6. 浏览器访问 `http://192.168.4.1/`，在"设置"页配 WiFi
7. 连上 WiFi 后，访问 ESP8266 的新 IP 即可使用

---

## 7. 常见问题

**Q: 烧录 ESP8266 时报 "Timed out waiting for packet header"？**
A: CC2530 的 TX 线干扰了 ESP8266 GPIO3 (RX)。烧录前先拔掉 CC2530 P0_3 → ESP8266 RX 这根线，烧完再插回。

**Q: 监控收不到 CC2530 日志？**
A: 1) 检查 TX 线是否插好；2) 确认 CC2530 已通电；3) 试试不同波特率（115200/57600/9600）；4) 点"复位 CC2530"触发启动日志。

**Q: 烧录 CC2530 失败？**
A: 1) 检查 D1/D2/D6 三根线是否接对；2) 确认 CC2530 GND 与 NodeMCU 共地；3) CC2530 需通电（3.3V）。

**Q: 忘了 WiFi 密码或连不上？**
A: 在"设置"页重新扫描+连接。若 WebUI 都进不去，删除 LittleFS 中的 /config.json 或重新烧录固件可重置。

**Q: 文件列表时间显示 1970 年？**
A: NTP 未同步。固件启动后通过 NTP 自动校准北京时间（UTC+8），通常连接 WiFi 后几秒内完成；AP 配网模式下无网络无法授时，连上 WiFi 后会自动同步。新上传的文件会带正确时间戳。

**Q: API 上传 .hex 报错 hex_not_supported？**
A: API（curl/Agent）仅接受 .bin，浏览器端上传 .hex 会自动转换但 API 不会。参考 2.3 节的 hex2bin 算法和 Python 实现，转换后再上传。

**Q: 烧录时 verify 参数不生效？**
A: API 烧录强制开启校验（verify=true），`verify` 参数被忽略。这是为了保证烧录正确性，避免 AI 跳过校验导致 CC2530 异常。校验失败会在 `burn.error` 字段报出。

**Q: "烧录完成重启烧录器"勾选项有什么用？**
A: 烧录成功后自动调用 /api/reboot 重启 ESP8266，可释放内存、重新初始化 GPIO 状态，适合连续多次烧录或烧录后立即监控的场景。
