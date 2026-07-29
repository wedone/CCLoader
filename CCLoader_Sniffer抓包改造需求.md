# CCLoader Sniffer 抓包功能改造需求

> **文档目的**：将 CCLoader（ESP8266 + CC2530 烧录器）改造为 Zigbee 抓包转发器，通过 WiFi 实时转发 CC2530 sniffer 固件的串口数据到 PC，替代 USB-TTL 直连方案。

---

## 1. 项目背景

### 1.1 上游项目

CCLoader 服务于 **Z-Stack 固件项目**（`D:\VC\Z-Stack`），该项目基于 TI Z-Stack 3.0.2 协议栈开发 CC2530 智能开关固件（HGZBSwitch）。

### 1.2 调试需求

当前正在诊断以下问题：
- CC2530 设备入网后交互不流畅（对比同硬件商业产品）
- S1 软复位后无法入网（已修复，需验证）
- 设备反复发送 Association Request 但不完成入网

这些问题需要通过 **Zigbee 抓包** 分析 MAC/NWK/APS/ZCL 各层协议交互来定位根因。

### 1.3 现有抓包方案（USB-TTL 直连）

目前使用独立的 CC2530+CC2592 模块烧录 ZBOSS sniffer 固件，通过 USB-TTL 直连 PC：

```
CC2530+CC2592 (sniffer固件) ──P0_3 UART0 TX──→ USB-TTL ──→ PC COM口
                                                    │
                                                    ▼
                                              Python sniffer_cli.py
                                                    │
                                                    ▼
                                              .pcap 文件 → Wireshark/tshark
```

**痛点**：
1. **PC 必须靠近 sniffer 模块**（USB 线缆长度限制，通常 <2 米）
2. **需要独立的 USB-TTL 适配器**（占用一个 USB 端口）
3. **无法远程抓包**（PC 不能随便移动）
4. **设备利用率低**（CCLoader 已有 ESP8266+CC2530 硬件，却闲置不用）

### 1.4 改造目标

利用 CCLoader 现有硬件（ESP8266 WiFi + CC2530），将 sniffer 模块的串口数据通过 WiFi 转发到 PC，实现**无线抓包**：

```
CC2530 sniffer固件 ──P0_3 UART0 TX──→ ESP8266 GPIO3 (RX)
                                          │
                                          ▼ WiFi
                                    PC (Python 脚本)
                                          │
                                          ▼
                                    .pcap → Wireshark
```

---

## 2. CCLoader 现状分析

### 2.1 硬件接线（已就绪）

CCLoader 现有硬件已完美匹配 sniffer 需求：

| ESP8266 GPIO | NodeMCU 丝印 | CC2530 引脚 | 当前用途 | sniffer 用途 |
|---|---|---|---|---|
| GPIO5 | D1 | Pin 7 (RESETn) | CC Debug 复位 | 不用（sniffer 模式下） |
| GPIO4 | D2 | Pin 3 (DC) | CC Debug 时钟 | 不用 |
| GPIO12 | D6 | Pin 4 (DD) | CC Debug 数据 | 不用 |
| **GPIO3** | **RX** | **P0_3 (UART0 TX)** | **监控 CC2530 日志** | **接收 sniffer 串口数据** ✅ |
| GND | GND | GND | 共地 | 共地 |
| 3V3 | 3V3 | VCC | 3.3V 供电 | 3.3V 供电 |

**关键点**：`GPIO3 (RX) ← P0_3 (UART0 TX)` 这根线已经接好，硬件零改动。

### 2.2 现有监控功能（/api/monitor）

CCLoader 已有 `MONITORING` 状态，可通过 `/api/monitor` 接口接收 CC2530 串口数据：

```json
POST /api/monitor
{"baud": 115200, "auto_reset": false}
```

数据通过两种方式获取：
- **SSE 推送**（端口 81）：`monitor_data` 事件，Base64 编码，攒 256 字节或 200ms 推送
- **HTTP 轮询**（端口 80）：`/api/monitor/buffer?since=N`，环形缓冲 8192 字节

### 2.3 现有功能的限制

| 限制项 | 现值 | 对 sniffer 的影响 | 严重度 |
|---|---|---|---|
| **环形缓冲大小** | 8192 字节 | Zigbee 流量大时 1-2 秒填满，丢包 | ❌ 致命 |
| **SSE 推送阈值** | 256 字节或 200ms | 延迟高，实时性差 | ⚠️ 中等 |
| **单次 loop 读取** | 128 字节 | 高流量时读取跟不上串口速度 | ❌ 致命 |
| **Base64 编码** | 33% 膨胀 | 浪费 WiFi 带宽，增加 CPU 负担 | ⚠️ 中等 |
| **监控波特率上限** | 230400 | 115200 够用 | ✅ OK |
| **无背压机制** | 缓冲满直接覆盖 | 静默丢包，pcap 文件残缺 | ❌ 致命 |

### 2.4 现有状态机

```
    ┌────────┐   POST /api/burn      ┌─────────┐
    │  IDLE  │ ─────────────────────▶│ BURNING │
    │        │◀─────────────────────│         │
    └────────┘   烧录完成/失败         └─────────┘
        │
        │ POST /api/monitor
        ▼
    ┌────────────┐
    │ MONITORING │
    └────────────┘
        │
        │ POST /api/stop
        ▼
    ┌────────┐
    │  IDLE  │
    └────────┘
```

---

## 3. 改造方案

### 3.1 新增 SNIFFING 状态

在现有三态状态机基础上新增 `SNIFFING` 状态，与 `MONITORING` 互斥：

```
    ┌────────┐                       ┌─────────┐
    │  IDLE  │◀───────烧录完成──────│ BURNING │
    │        │                       └─────────┘
    └────────┘
        │ │
        │ │ POST /api/monitor        │ POST /api/sniffer/start
        │ ▼                          │ ▼
        │ ┌────────────┐    ┌─────────────┐
        │ │ MONITORING │    │  SNIFFING   │
        │ └────────────┘    └─────────────┘
        │     │                  │
        └─────┴──────POST /api/stop──────────┘
```

**状态互斥规则**：
- `SNIFFING` 与 `BURNING`、`MONITORING` 互斥
- 进入 `SNIFFING` 前必须处于 `IDLE`
- `POST /api/stop` 可从 `SNIFFING` 回到 `IDLE`

### 3.2 核心改造点

| 改造项 | 现状 | 目标 | 说明 |
|---|---|---|---|
| **环形缓冲** | 8192 字节 | **32768 字节（32KB）** | 4 倍扩容，容纳 ~500 个典型 Zigbee 包 |
| **推送阈值** | 256 字节/200ms | **64 字节/50ms** | 更低延迟，接近实时 |
| **单次 loop 读取** | 128 字节 | **512 字节** | 4 倍提升读取吞吐 |
| **数据编码** | Base64 | **二进制透传** | 省去编码开销，零膨胀 |
| **背压机制** | 无（静默覆盖） | **丢包计数 + 标记** | pcap 完整性可追踪 |
| **流式接口** | 无 | **HTTP chunked 流** | 持续推送，无需轮询 |

### 3.3 ZBOSS Sniffer 串口协议（关键）

CC2530 烧录的 ZBOSS sniffer 固件通过 UART0（P0_3，115200 8N1）输出数据，格式如下：

#### 3.3.1 PC → sniffer（下行，仅 1 字节）

```
[通道号]  # 1 字节，值 11-26，启动该通道抓包
```

#### 3.3.2 sniffer → PC（上行，变长包）

```
┌──────┬──────┬─────────┬──────────────────────────────┐
│ len  │ type │ tail(2) │ IEEE 802.15.4 帧 + CRC状态   │
│ 1B   │ 1B   │ 2B      │ (len-4) 字节                 │
└──────┴──────┴─────────┴──────────────────────────────┘
```

| 字段 | 偏移 | 长度 | 说明 |
|---|---|---|---|
| `len` | 0 | 1 | 总长度（含 4 字节包头），范围 5-127 |
| `type` | 1 | 1 | 0=OK（正常包），1=TOO_BIG（超长），2=OVERFLOW（溢出） |
| `tail` | 2 | 2 | 保留，通常为 0x0000 |
| `payload` | 4 | len-4 | IEEE 802.15.4 帧 + 最后 1 字节 CRC 状态 |

**payload 最后 1 字节**：bit7=1 表示 CRC 校验通过，bit7=0 表示 CRC 错误

#### 3.3.3 pcap 文件格式（PC 端生成）

ESP8266 只需**透传原始字节**，PC 端 Python 脚本负责：
1. 解析 ZBOSS 包头（4 字节）
2. 提取 IEEE 802.15.4 帧
3. 移除最后 1 字节 CRC 状态
4. 封装为 pcap 记录（DLT_IEEE802_15_4_NOFCS=230）

**pcap 全局头（24 字节）**：
```
magic(4) | version_major(2) | version_minor(2) | thiszone(4) | sigfigs(4) | snaplen(4) | dlt(4)
0xa1b2c3d4 | 0x0002          | 0x0004          | 0           | 0          | 65535     | 230
```

**pcap 包记录头（16 字节）**：
```
ts_sec(4) | ts_usec(4) | caplen(4) | origlen(4)
```

### 3.4 数据流架构

```
CC2530 sniffer ──UART 115200──→ ESP8266 GPIO3
                                    │
                                    ▼ Serial.read()
                              环形缓冲 32KB
                                    │
                                    ▼ HTTP chunked
                              /api/sniffer/stream
                                    │
                                    ▼ WiFi
                              PC Python 脚本
                                    │
                                    ▼ 解析ZBOSS协议
                              .pcap 文件
                                    │
                                    ▼
                              Wireshark / tshark
```

---

## 4. API 设计

### 4.1 新增接口列表

| 方法 | 路径 | 功能 | 状态要求 | 返回码 |
|---|---|---|---|---|
| POST | `/api/sniffer/start` | 启动 sniffer 模式 | IDLE | 200 / 400 / 409 |
| POST | `/api/sniffer/channel` | 切换通道 | SNIFFING | 200 / 409 |
| GET | `/api/sniffer/stream` | 获取流式数据（chunked） | SNIFFING | 200 / 409 |
| GET | `/api/sniffer/status` | sniffer 状态 + 丢包统计 | 任意 | 200 |

### 4.2 POST /api/sniffer/start

启动 sniffer 模式，切换 Serial 到指定波特率，发送通道号给 CC2530。

**请求体**：
```json
{
  "channel": 11,
  "baud": 115200
}
```

| 字段 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| `channel` | int | 否 | 11 | Zigbee 通道号（11-26） |
| `baud` | int | 否 | 115200 | 串口波特率，固定 115200 |

**成功响应**（200）：
```json
{
  "success": true,
  "channel": 11,
  "baud": 115200,
  "buffer_size": 32768
}
```

**错误响应**：
- 状态非 IDLE（409）：`{"error": "busy"}`
- 通道号无效（400）：`{"error": "invalid channel"}`

**行为**：
1. `Serial.end()` → `Serial.begin(115200)`
2. 清空环形缓冲
3. 发送 1 字节通道号给 CC2530：`Serial.write(channel)`
4. 进入 `SNIFFING` 状态
5. 推送 SSE `sniffer_start` 事件

### 4.3 POST /api/sniffer/channel

切换抓包通道（无需重启 sniffer）。

**请求体**：
```json
{"channel": 15}
```

**响应**（200）：
```json
{"success": true, "channel": 15}
```

**行为**：发送 1 字节新通道号给 CC2530，清空缓冲。

### 4.4 GET /api/sniffer/stream

**核心接口**。HTTP chunked 流式传输，持续推送 sniffer 原始字节。

**响应头**：
```
HTTP/1.1 200 OK
Content-Type: application/octet-stream
Transfer-Encoding: chunked
Cache-Control: no-cache
Connection: keep-alive
```

**响应体**（chunked，持续推送）：
```
<chunk1><chunk2><chunk3>...
```

每个 chunk 是 sniffer 接收到的**原始字节**（包含 ZBOSS 包头），无任何编码。

**客户端断开**：ESP8266 检测到客户端断开后，可选择保持 sniffer 运行或自动停止（建议保持，允许多次连接）。

**背压处理**：
- 如果 WiFi 发送速度 < 串口接收速度，环形缓冲逐渐填满
- 缓冲满时，**丢弃最旧的数据**，`dropped_bytes` 计数器递增
- 每 1024 字节丢弃时，推送一个特殊标记包（见 4.6）

### 4.5 GET /api/sniffer/status

获取 sniffer 状态和统计。

**响应**：
```json
{
  "active": true,
  "channel": 11,
  "baud": 115200,
  "buffer_used": 4096,
  "buffer_size": 32768,
  "bytes_received": 102400,
  "bytes_sent": 98304,
  "dropped_bytes": 4096,
  "drop_count": 4,
  "uptime": 60
}
```

| 字段 | 说明 |
|---|---|
| `active` | 是否处于 SNIFFING 状态 |
| `channel` | 当前通道 |
| `buffer_used` | 当前缓冲已用字节 |
| `bytes_received` | 累计接收字节数 |
| `bytes_sent` | 累计发送字节数 |
| `dropped_bytes` | 缓冲满丢弃的字节数 |
| `drop_count` | 丢弃次数 |
| `uptime` | sniffer 运行秒数 |

### 4.6 丢包标记协议

当缓冲溢出丢包时，在流中插入一个**特殊标记包**，PC 端可识别并标记 pcap：

```
ZBOSS 包头格式，但 type=0xFF 表示丢包标记：
┌──────┬────────┬─────────┬──────────────────┐
│ len  │ type   │ tail    │ dropped_bytes(4) │
│ 1B   │ 1B=0xFF│ 2B=0x00 │ 4 字节大端       │
└──────┴────────┴─────────┴──────────────────┘
len = 8 (4包头 + 4数据)
```

PC 端 Python 脚本收到 type=0xFF 时，在 pcap 中插入一个注释包或跳过。

---

## 5. 实现要点

### 5.1 环形缓冲设计

```cpp
// 推荐：双缓冲或环形缓冲
#define SNIFFER_BUFFER_SIZE 32768  // 32KB

uint8_t sniffer_buffer[SNIFFER_BUFFER_SIZE];
volatile uint16_t sniffer_head = 0;  // 写入位置（中断/loop）
volatile uint16_t sniffer_tail = 0;  // 读取位置（HTTP stream）
volatile uint32_t sniffer_total_rx = 0;
volatile uint32_t sniffer_total_drop = 0;
volatile uint32_t sniffer_drop_count = 0;

// 写入（loop 中从 Serial 读取后写入）
void sniffer_write(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint16_t next = (sniffer_head + 1) % SNIFFER_BUFFER_SIZE;
        if (next == sniffer_tail) {
            // 缓冲满，丢弃最旧数据
            sniffer_tail = (sniffer_tail + 1) % SNIFFER_BUFFER_SIZE;
            sniffer_total_drop++;
            if (++sniffer_drop_accum == 0) {
                // 累计 256 字节丢弃，插入标记包
                // ...
            }
        }
        sniffer_buffer[sniffer_head] = data[i];
        sniffer_head = next;
        sniffer_total_rx++;
    }
}

// 读取（HTTP stream 客户端）
size_t sniffer_read(uint8_t *out, size_t max_len) {
    size_t count = 0;
    while (count < max_len && sniffer_tail != sniffer_head) {
        out[count++] = sniffer_buffer[sniffer_tail];
        sniffer_tail = (sniffer_tail + 1) % SNIFFER_BUFFER_SIZE;
    }
    return count;
}
```

### 5.2 loop() 中的串口读取

```cpp
void sniffer_loop() {
    if (g_state != SNIFFING) return;
    
    // 每次最多读 512 字节，避免占用太久
    size_t available = Serial.available();
    if (available == 0) return;
    if (available > 512) available = 512;
    
    uint8_t tmp[512];
    size_t n = Serial.readBytes(tmp, available);
    if (n > 0) {
        sniffer_write(tmp, n);
    }
}
```

### 5.3 HTTP chunked 流式推送

```cpp
void handleSnifferStream() {
    if (g_state != SNIFFING) {
        server.send(409, "application/json", "{\"error\":\"not sniffing\"}");
        return;
    }
    
    // 发送 chunked 响应头
    WiFiClient client = server.client();
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: application/octet-stream");
    client.println("Transfer-Encoding: chunked");
    client.println("Cache-Control: no-cache");
    client.println("Connection: keep-alive");
    client.println();
    
    uint8_t buf[1024];
    while (client.connected() && g_state == SNIFFING) {
        size_t n = sniffer_read(buf, sizeof(buf));
        if (n > 0) {
            // chunked 格式: <hex长度>\r\n<data>\r\n
            client.printf("%X\r\n", n);
            client.write(buf, n);
            client.print("\r\n");
            sniffer_total_sent += n;
        } else {
            // 无数据，短暂等待
            delay(5);
        }
        // 喂狗
        if (sniffer_total_rx % 4096 == 0) yield();
    }
    
    client.println("0\r\n\r\n");  // 结束 chunk
}
```

### 5.4 状态机集成

在现有 `loop()` 中加入 sniffer 处理：

```cpp
void loop() {
    server.handleClient();
    
    if (g_state == BURNING) {
        // 现有烧录逻辑
        handleBurn();
    } else if (g_state == MONITORING) {
        // 现有监控逻辑
        handleMonitor();
    } else if (g_state == SNIFFING) {
        // 新增 sniffer 逻辑
        sniffer_loop();
    }
    
    handleSSE();
    yield();
}
```

### 5.5 互斥规则

在 `/api/burn`、`/api/monitor`、`/api/sniffer/start` 中检查状态：

```cpp
void handleSnifferStart() {
    if (g_state != IDLE) {
        server.send(409, "application/json", "{\"error\":\"busy\"}");
        return;
    }
    // ... 启动 sniffer
    g_state = SNIFFING;
}
```

---

## 6. PC 端 Python 脚本（参考）

PC 端脚本负责接收 ESP8266 的流式数据，解析 ZBOSS 协议，生成 pcap 文件。

```python
#!/usr/bin/env python3
"""
CCLoader Sniffer 客户端 - 通过 WiFi 接收 sniffer 数据并生成 pcap
用法: python ccloader_sniffer.py <ESP8266_IP> [通道号] [输出文件]
示例: python ccloader_sniffer.py 10.0.0.147 11 capture.pcap
"""
import sys, os, time, struct, requests

DLT_IEEE802_15_4_NOFCS = 230

def write_pcap_header(f):
    f.write(struct.pack('<IHHiIII',
        0xa1b2c3d4, 2, 4, 0, 0, 65535, DLT_IEEE802_15_4_NOFCS))

def write_pcap_packet(f, data, ts=None):
    if ts is None:
        ts = time.time()
    sec = int(ts)
    usec = int((ts - sec) * 1000000)
    f.write(struct.pack('<IIII', sec, usec, len(data), len(data)))
    f.write(data)
    f.flush()

def parse_zboss_stream(stream_buf, pos):
    """解析 ZBOSS 包，返回 (ieee_frame, next_pos) 或 (None, pos)"""
    if pos + 4 > len(stream_buf):
        return None, pos
    pkt_len = stream_buf[pos]
    pkt_type = stream_buf[pos + 1]
    if pkt_len < 5 or pos + pkt_len > len(stream_buf):
        return None, pos
    if pkt_type == 0xFF:  # 丢包标记
        dropped = struct.unpack('>I', stream_buf[pos+4:pos+8])[0]
        print(f"[WARN] 丢失 {dropped} 字节")
        return None, pos + pkt_len
    if pkt_type != 0:  # 非 OK，跳过
        return None, pos + pkt_len
    ieee_data = stream_buf[pos + 4 : pos + pkt_len]
    if len(ieee_data) > 1:
        ieee_data = ieee_data[:-1]  # 移除 CRC 状态字节
    return ieee_data, pos + pkt_len

def main():
    ip = sys.argv[1] if len(sys.argv) > 1 else "10.0.0.147"
    channel = int(sys.argv[2]) if len(sys.argv) > 2 else 11
    output = sys.argv[3] if len(sys.argv) > 3 else f"capture_{time.strftime('%Y%m%d_%H%M%S')}.pcap"
    
    print(f"=== CCLoader Sniffer ===")
    print(f"ESP8266: {ip}")
    print(f"通道: {channel}")
    print(f"输出: {output}")
    
    # 1. 启动 sniffer
    r = requests.post(f"http://{ip}/api/sniffer/start", json={"channel": channel})
    if r.status_code != 200:
        print(f"[ERROR] 启动失败: {r.text}")
        sys.exit(1)
    print(f"[OK] sniffer 已启动: {r.json()}")
    
    # 2. 流式接收
    stream_buf = bytearray()
    pkt_count = 0
    start_time = time.time()
    
    with open(output, 'wb') as f:
        write_pcap_header(f)
        print(f"[RX] 监听中... (Ctrl+C 停止)")
        
        try:
            with requests.get(f"http://{ip}/api/sniffer/stream", stream=True, timeout=None) as r:
                for chunk in r.iter_content(chunk_size=1024):
                    if not chunk:
                        continue
                    stream_buf.extend(chunk)
                    
                    pos = 0
                    while pos < len(stream_buf):
                        ieee, next_pos = parse_zboss_stream(stream_buf, pos)
                        if ieee is None:
                            if next_pos == pos:
                                break
                            pos = next_pos
                            continue
                        write_pcap_packet(f, ieee)
                        pkt_count += 1
                        pos = next_pos
                    if pos > 0:
                        del stream_buf[:pos]
        except KeyboardInterrupt:
            pass
    
    elapsed = time.time() - start_time
    print(f"\n=== 抓包结束 ===")
    print(f"时长: {elapsed:.1f}s | 包数: {pkt_count} | 文件: {output}")
    
    # 3. 停止 sniffer
    requests.post(f"http://{ip}/api/stop")

if __name__ == '__main__':
    main()
```

---

## 7. CC2530 sniffer 固件说明

### 7.1 固件来源

CC2530 sniffer 固件基于 ZBOSS v1.0 SDK 编译（`D:\VC\zboss-v1.0`），已做以下修改：

1. **自动启动通道 11**：上电后自动开始抓包，无需 PC 发送通道号
2. **CRC 状态处理**：正确传递 CRC 校验结果（最后一字节 bit7）
3. **UART0 输出**：通过 P0_3（UART0 TX）115200 8N1 输出

### 7.2 sniffer 固件与 CCLoader 的配合

CCLoader 的 CC2530 烧录的是 **HGZBSwitch 固件**（Z-Stack Router），不是 sniffer 固件。

**使用模式**：
- **抓包模式**：CCLoader 的 CC2530 先烧录 sniffer 固件 → 进入 SNIFFING 状态抓包
- **烧录模式**：CCLoader 的 CC2530 烧录 HGZBSwitch 固件 → 用于设备固件升级

两种模式互斥，通过状态机保证。

> **注意**：如果 CCLoader 的 CC2530 烧录的是 sniffer 固件，则 `/api/burn` 不能用于烧录 HGZBSwitch（会覆盖 sniffer 固件）。建议准备**两个 CCLoader**：一个专用于抓包（烧 sniffer 固件），一个用于烧录设备固件。

---

## 8. 验证方法

### 8.1 功能验证

1. **启动 sniffer**：
   ```bash
   curl -X POST http://10.0.0.147/api/sniffer/start -H "Content-Type: application/json" -d '{"channel":11}'
   ```

2. **检查状态**：
   ```bash
   curl http://10.0.0.147/api/sniffer/status
   # 应返回 active:true, channel:11
   ```

3. **流式接收**：
   ```bash
   curl http://10.0.0.147/api/sniffer/stream -o test.bin
   # 观察 test.bin 是否增长
   ```

4. **PC 端抓包**：
   ```bash
   python ccloader_sniffer.py 10.0.0.147 11 test.pcap
   # 在 z2m 协调器附近操作设备，观察是否抓到包
   ```

5. **Wireshark 验证**：用 Wireshark 打开 test.pcap，应能看到 IEEE 802.15.4 / Zigbee NWK / ZCL 帧

### 8.2 性能验证

| 指标 | 目标 | 验证方法 |
|---|---|---|
| **吞吐** | ≥ 5 KB/s（典型 Zigbee 流量） | `/api/sniffer/status` 的 `bytes_received` 增速 |
| **丢包率** | < 1%（正常流量） | `dropped_bytes / bytes_received` |
| **延迟** | < 500ms | PC 端时间戳 vs 设备实际操作时间 |
| **缓冲利用率** | < 50% | `buffer_used / buffer_size` |

### 8.3 对比验证

与现有 USB-TTL 直连方案对比：
1. 同时用 USB-TTL（COM4）和 CCLoader（WiFi）抓同一通道
2. 对比两个 pcap 文件的包数量、内容
3. 应该基本一致（CCLoader 可能多丢几个包，但不应大量丢失）

---

## 9. 注意事项

### 9.1 ESP8266 性能限制

- **CPU**：ESP8266 80MHz，处理 115200 串口 + WiFi 可能达到瓶颈
- **内存**：32KB 环形缓冲占用约 1/3 可用 RAM，需测试是否导致 OOM
- **WiFi 稳定性**：WiFi 断连时数据会堆积在缓冲，缓冲满后丢包

### 9.2 串口冲突

- `SNIFFING` 状态下 `GPIO3 (RX)` 专用于接收 sniffer 数据
- 不能同时使用 `/api/monitor`（互斥）
- OTA 升级时必须先 `/api/stop` 停止 sniffer

### 9.3 CC2530 固件要求

- CCLoader 的 CC2530 必须烧录 **ZBOSS sniffer 固件**（不是 HGZBSwitch）
- sniffer 固件波特率必须为 115200（与 CCLoader 配置一致）
- sniffer 固件已配置为上电自动启动通道 11

### 9.4 通道切换

- ZBOSS sniffer 固件支持运行时切换通道（PC 发送 1 字节通道号）
- 切换通道会清空 CC2530 的 RF FIFO，可能导致短暂丢包
- 建议在抓包前确定 z2m 使用的通道（通常 11、15、20、25 之一）

### 9.5 SSE 事件

新增 SSE 事件类型（端口 81）：

| 事件 | 触发时机 | 数据 |
|---|---|---|
| `sniffer_start` | 进入 SNIFFING | `{"type":"sniffer_start","channel":11}` |
| `sniffer_stop` | 退出 SNIFFING | `{"type":"sniffer_stop"}` |
| `sniffer_stats` | 每 10 秒推送统计 | `{"type":"sniffer_stats","rx":1024,"drop":0}` |

---

## 10. 实现优先级

| 优先级 | 功能 | 说明 |
|---|---|---|
| **P0** | `/api/sniffer/start` + 状态机 | 基础功能 |
| **P0** | `/api/sniffer/stream` chunked 流 | 核心数据传输 |
| **P0** | 32KB 环形缓冲 | 防丢包 |
| **P1** | `/api/sniffer/status` | 状态监控 |
| **P1** | `/api/sniffer/channel` 切换通道 | 便利性 |
| **P2** | 丢包标记包（type=0xFF） | 可追溯性 |
| **P2** | SSE 事件推送 | 实时通知 |
| **P3** | `/api/stop` 从 SNIFFING 回 IDLE | 兼容现有接口 |

---

## 11. 参考资源

| 资源 | 路径 / 链接 | 说明 |
|---|---|---|
| CCLoader 主固件 | `D:\VC\CCLoader\src\CCLoader.ino` | 现有代码基础 |
| CCLoader API 文档 | `D:\VC\CCLoader\Documents\API参考.md` | 现有 API 规范 |
| CCLoader 硬件设计 | `D:\VC\CCLoader\Documents\硬件设计.md` | 引脚接线 |
| ZBOSS sniffer 固件源码 | `D:\VC\zboss-v1.0\devtools\sniffer\` | sniffer 固件 |
| ZBOSS sniffer 工具 | `D:\VC\zboss-v1.0\devtools\sniffer\zb_sniffer_tools.c` | 协议实现 |
| 现有 USB-TTL 抓包脚本 | `D:\VC\Z-Stack\tools\sniffer_cli.py` | PC 端参考 |
| 抓包分析脚本 | `D:\VC\Z-Stack\tools\analyze_capture.py` | pcap 分析 |
| pcap 格式规范 | https://wiki.wireshark.org/Development/LibpcapFile | DLT=230 |
| IEEE 802.15.4 规范 | https://standards.ieee.org/standard/802_15_4-2020.html | MAC 层 |

---

## 12. 附录：ZBOSS sniffer 数据包示例

### 12.1 典型 Beacon Request 包

```
原始串口字节:
09 00 00 00 08 03 FF FF FF 00 07

解析:
- len = 0x09 (9 字节)
- type = 0x00 (OK)
- tail = 0x00 0x00
- payload (5 字节):
  08 03 FF FF FF 00 07
  - IEEE 802.15.4 帧: 08 03 FF FF FF 00 07 (前 6 字节)
  - CRC 状态: 0x07 (bit7=0, CRC 错误；此包为广播，无 CRC)

pcap 写入 (移除 CRC 状态字节):
08 03 FF FF FF 00
```

### 12.2 典型 Data 包（含 ZCL）

```
原始串口字节:
42 00 00 00 61 88 XX 62 1A DA 7C 7D 28 48 02 00 00 7D 28 1E 00

解析:
- len = 0x42 (66 字节)
- type = 0x00 (OK)
- tail = 0x00 0x00
- payload (62 字节): IEEE 802.15.4 帧 (61 字节) + CRC 状态 (1 字节)

pcap 写入 (61 字节，移除最后 CRC 状态):
61 88 XX 62 1A DA 7C ...
```

---

## 13. 验收标准

1. ✅ `POST /api/sniffer/start` 能成功启动，CC2530 sniffer 开始抓包
2. ✅ `GET /api/sniffer/stream` 能持续返回数据，延迟 < 500ms
3. ✅ PC 端 Python 脚本能正确解析 ZBOSS 协议，生成可用 pcap
4. ✅ Wireshark 能打开 pcap，显示 IEEE 802.15.4 / Zigbee 帧
5. ✅ 32KB 缓冲在正常流量下不丢包（`dropped_bytes=0`）
6. ✅ `/api/burn`、`/api/monitor` 在 SNIFFING 状态下返回 409
7. ✅ `/api/stop` 能正确停止 sniffer，回到 IDLE

---

**文档版本**：v1.0
**创建日期**：2026-07-30
**目标工作区**：`D:\VC\CCLoader`
**上游项目**：`D:\VC\Z-Stack`（Zigbee 抓包分析）
