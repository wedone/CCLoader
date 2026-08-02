/******************************************************************************
 * CCLoader WebUI - 基于 ESP32-SOLO-1 的 CC2530 烧录+监控一体机
 *
 * 接线（ESP32-SOLO-1 开发板）：
 *   IO5   -> CC Pin 7 (RESETn)
 *   IO4   -> CC Pin 3 (DC)
 *   IO12  -> CC Pin 4 (DD)
 *   IO16  -> CC P0_3   (UART0 TX)  监控/sniffer 用（Serial2 RX，U2RXD 默认引脚）
 *   GND   -> CC GND
 *   IO2   -> 板载 LED2（状态指示）
 *   IO22  -> 板载 CH340C RXD（Serial TX，调试日志）
 *   IO23  -> 板载 CH340C TXD（Serial RX，调试日志）
 *
 * 工作模式：
 *   - IDLE：WiFi+HTTP+SSE 在线，等待浏览器操作
 *   - BURNING：从 LittleFS 读取 BIN 烧录到 CC2530，进度通过 SSE 推送
 *   - MONITORING：Serial2 切换到 CC2530 波特率，接收 P0_3 日志并通过 SSE 推送
 *   - SNIFFING：Serial2 接收 ZBOSS sniffer 原始帧，HTTP chunked 透传到 PC
 *
 * 双串口架构：
 *   - Serial  (UART0): 重映射到 IO22/IO23，接板载 USB，做调试日志（始终可用）
 *   - Serial2 (UART2): IO16 RX，接 CC2530 P0_3，做监控/sniffer 数据接收
 *   物理分离解决了 ESP8266 时代 Serial TX 写入影响 RX 接收的限制
 *
 * 无外部库依赖：
 *   - HTTP/WebServer: WebServer (端口 80)
 *   - 实时推送:       原生 WiFiServer SSE (端口 81)
 *   - JSON:           String 手工拼接/解析（仅顶层简单字段）
 ******************************************************************************/

/*
 Copyright (c) 2012-2014 RedBearLab
 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:
 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 ******************************************************************************/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPUpdateServer.h>
#include <uri/UriBraces.h>
#include <LittleFS.h>
#include <FS.h>
#include <esp_system.h>  // esp_reset_reason() 重启原因诊断
#include "web_assets.h"  // 内嵌 WebUI 静态资源（OTA 升级时一并更新）

/******************************************************************************
 * 固件版本与编译标识（发版时修改 FIRMWARE_VERSION，BUILD_TIME 自动生成）
 *****************************************************************************/
#define FIRMWARE_VERSION "v1.6"
// 编译日期时间戳：由编译器 __DATE__/__TIME__ 宏自动生成（如 "Jul 30 2026 16:36:35"）
// 用于区分同版本号的不同编译产物，无需手动维护
#define BUILD_TIME (__DATE__ " " __TIME__)

/******************************************************************************
 * DEFINES - CC Debug 协议（原样保留）
 *****************************************************************************/
// Start addresses on DUP (Increased buffer size improves performance)
#define ADDR_BUF0                   0x0000 // Buffer (512 bytes)
#define ADDR_DMA_DESC_0             0x0200 // DMA descriptors (8 bytes)
#define ADDR_DMA_DESC_1             (ADDR_DMA_DESC_0 + 8)

// DMA channels used on DUP
#define CH_DBG_TO_BUF0              0x01   // Channel 0
#define CH_BUF0_TO_FLASH            0x02   // Channel 1

// Debug commands
#define CMD_CHIP_ERASE              0x10
#define CMD_WR_CONFIG               0x19
#define CMD_RD_CONFIG               0x24
#define CMD_READ_STATUS             0x30
#define CMD_RESUME                  0x4C
#define CMD_DEBUG_INSTR_1B          (0x54|1)
#define CMD_DEBUG_INSTR_2B          (0x54|2)
#define CMD_DEBUG_INSTR_3B          (0x54|3)
#define CMD_BURST_WRITE             0x80
#define CMD_GET_CHIP_ID             0x68

// Debug status bitmasks
#define STATUS_CHIP_ERASE_BUSY_BM   0x80
#define STATUS_PCON_IDLE_BM         0x40
#define STATUS_CPU_HALTED_BM        0x20
#define STATUS_PM_ACTIVE_BM         0x10
#define STATUS_HALT_STATUS_BM       0x08
#define STATUS_DEBUG_LOCKED_BM      0x04
#define STATUS_OSC_STABLE_BM        0x02
#define STATUS_STACK_OVERFLOW_BM    0x01

// DUP registers (XDATA space address)
#define DUP_DBGDATA                 0x6260
#define DUP_FCTL                    0x6270
#define DUP_FADDRL                  0x6271
#define DUP_FADDRH                  0x6272
#define DUP_FWDATA                  0x6273
#define DUP_CLKCONSTA               0x709E
#define DUP_CLKCONCMD               0x70C6
#define DUP_MEMCTR                  0x70C7
#define DUP_DMA1CFGL                0x70D2
#define DUP_DMA1CFGH                0x70D3
#define DUP_DMA0CFGL                0x70D4
#define DUP_DMA0CFGH                0x70D5
#define DUP_DMAARM                  0x70D6

// Utility macros
#define LOBYTE(w)           ((unsigned char)(w))
#define HIBYTE(w)           ((unsigned char)(((unsigned short)(w) >> 8) & 0xFF))

// ===== 引脚配置（ESP32-SOLO-1）=====
int DD = 12;     // IO12  - CC Debug 数据（双向）
int DC = 4;      // IO4   - CC Debug 时钟
int RESET = 5;   // IO5   - CC Debug 复位
int LED = 2;     // IO2   - 板载 LED2（状态指示）

// ===== 串口配置（ESP32-SOLO-1）=====
// Serial  (UART0): 重映射到 IO22(TX)/IO23(RX)，接板载 CH340C USB-TTL，做调试日志输出
// Serial2 (UART2): IO16(RX)/IO17(TX)，接 CC2530 P0_3 (UART0 TX)，做监控/sniffer 数据接收
//   物理分离调试串口与 CC2530 数据串口，监控/sniffer 模式下调试日志仍可正常输出
//   引脚选择：IO16/IO17 是 UART2 的 IO MUX 默认引脚（U2RXD/U2TXD），无冲突
//   IO1/IO3 虽是开发板丝印 TX/RX，但它们是 UART0 的 IO MUX 默认引脚（U0TXD/U0RXD），
//   即使 UART0 重映射到 IO22/IO23，IO3 仍可能被 UART0 IO MUX 影响，用作 UART2 RX 时存在冲突
//   IO16/IO17 是 UART2 专用引脚，通过 IO MUX 直连 UART2，最可靠
#define CC_SERIAL_RX  16   // Serial2 RX = IO16 (U2RXD) ← CC2530 P0_3
#define CC_SERIAL_TX  17   // Serial2 TX = IO17 (U2TXD)，未接 CC2530，仅 RX 用

/******************************************************************************
 * VARIABLES - DUP DMA descriptor（原样保留）
 *****************************************************************************/
const unsigned char dma_desc_0[8] =
{
    HIBYTE(DUP_DBGDATA), LOBYTE(DUP_DBGDATA),
    HIBYTE(ADDR_BUF0), LOBYTE(ADDR_BUF0),
    0, 0, 31, 0x11
};
const unsigned char dma_desc_1[8] =
{
    HIBYTE(ADDR_BUF0), LOBYTE(ADDR_BUF0),
    HIBYTE(DUP_FWDATA), LOBYTE(DUP_FWDATA),
    0, 0, 18, 0x42
};

/******************************************************************************
 * CC Debug 协议函数（原样保留，完全不变）
 *****************************************************************************/
inline void write_debug_byte(unsigned char data) __attribute__((always_inline));
inline void write_debug_byte(unsigned char data)
{
    unsigned char i;
    for (i = 0; i < 8; i++)
    {
        digitalWrite(DC, HIGH);
        if(data & 0x80) digitalWrite(DD, HIGH);
        else            digitalWrite(DD, LOW);
        data <<= 1;
        digitalWrite(DC, LOW);
    }
}

inline unsigned char read_debug_byte(void) __attribute__((always_inline));
inline unsigned char read_debug_byte(void)
{
    unsigned char i;
    unsigned char data = 0x00;
    for (i = 0; i < 8; i++)
    {
        digitalWrite(DC, HIGH);
        data <<= 1;
        if(HIGH == digitalRead(DD)) data |= 0x01;
        digitalWrite(DC, LOW);
    }
    return data;
}

inline unsigned char wait_dup_ready(void) __attribute__((always_inline));
inline unsigned char wait_dup_ready(void)
{
    unsigned int count = 0;
    while ((HIGH == digitalRead(DD)) && count < 16)
    {
        read_debug_byte();
        count++;
    }
    return (count == 16) ? 0 : 1;
}

unsigned char debug_command(unsigned char cmd, unsigned char *cmd_bytes,
                            unsigned short num_cmd_bytes)
{
    unsigned short i;
    unsigned char output = 0;
    pinMode(DD, OUTPUT);
    write_debug_byte(cmd);
    for (i = 0; i < num_cmd_bytes; i++) write_debug_byte(cmd_bytes[i]);
    pinMode(DD, INPUT);
    digitalWrite(DD, HIGH);
    wait_dup_ready();
    output = read_debug_byte();
    pinMode(DD, OUTPUT);
    return output;
}

void debug_init(void)
{
    digitalWrite(DD, LOW);
    digitalWrite(DC, LOW);
    digitalWrite(RESET, LOW);
    delay(10);
    digitalWrite(DC, HIGH); delay(10);
    digitalWrite(DC, LOW);  delay(10);
    digitalWrite(DC, HIGH); delay(10);
    digitalWrite(DC, LOW);  delay(10);
    digitalWrite(RESET, HIGH);
    delay(10);
}

unsigned char read_chip_id(void)
{
    unsigned char id = 0;
    pinMode(DD, OUTPUT);
    delay(1);
    write_debug_byte(CMD_GET_CHIP_ID);
    pinMode(DD, INPUT);
    digitalWrite(DD, HIGH);
    delay(1);
    if(wait_dup_ready() == 1)
    {
        id = read_debug_byte();
        read_debug_byte();
    }
    pinMode(DD, OUTPUT);
    return id;
}

void burst_write_block(unsigned char *src, unsigned short num_bytes)
{
    unsigned short i;
    pinMode(DD, OUTPUT);
    write_debug_byte(CMD_BURST_WRITE | HIBYTE(num_bytes));
    write_debug_byte(LOBYTE(num_bytes));
    for (i = 0; i < num_bytes; i++) write_debug_byte(src[i]);
    pinMode(DD, INPUT);
    digitalWrite(DD, HIGH);
    wait_dup_ready();
    read_debug_byte();
    pinMode(DD, OUTPUT);
}

void chip_erase(void)
{
    volatile unsigned char status;
    debug_command(CMD_CHIP_ERASE, 0, 0);
    do {
        status = debug_command(CMD_READ_STATUS, 0, 0);
    } while((status & STATUS_CHIP_ERASE_BUSY_BM));
}

void write_xdata_memory_block(unsigned short address,
                              const unsigned char *values,
                              unsigned short num_bytes)
{
    unsigned char instr[3];
    unsigned short i;
    instr[0] = 0x90;
    instr[1] = HIBYTE(address);
    instr[2] = LOBYTE(address);
    debug_command(CMD_DEBUG_INSTR_3B, instr, 3);
    for (i = 0; i < num_bytes; i++)
    {
        instr[0] = 0x74;
        instr[1] = values[i];
        debug_command(CMD_DEBUG_INSTR_2B, instr, 2);
        instr[0] = 0xF0;
        debug_command(CMD_DEBUG_INSTR_1B, instr, 1);
        instr[0] = 0xA3;
        debug_command(CMD_DEBUG_INSTR_1B, instr, 1);
    }
}

void write_xdata_memory(unsigned short address, unsigned char value)
{
    unsigned char instr[3];
    instr[0] = 0x90;
    instr[1] = HIBYTE(address);
    instr[2] = LOBYTE(address);
    debug_command(CMD_DEBUG_INSTR_3B, instr, 3);
    instr[0] = 0x74;
    instr[1] = value;
    debug_command(CMD_DEBUG_INSTR_2B, instr, 2);
    instr[0] = 0xF0;
    debug_command(CMD_DEBUG_INSTR_1B, instr, 1);
}

unsigned char read_xdata_memory(unsigned short address)
{
    unsigned char instr[3];
    instr[0] = 0x90;
    instr[1] = HIBYTE(address);
    instr[2] = LOBYTE(address);
    debug_command(CMD_DEBUG_INSTR_3B, instr, 3);
    instr[0] = 0xE0;
    return debug_command(CMD_DEBUG_INSTR_1B, instr, 1);
}

void read_flash_memory_block(unsigned char bank, unsigned short flash_addr,
                             unsigned short num_bytes, unsigned char *values)
{
    unsigned char instr[3];
    unsigned short i;
    unsigned short xdata_addr = (0x8000 + flash_addr);
    write_xdata_memory(DUP_MEMCTR, bank);
    instr[0] = 0x90;
    instr[1] = HIBYTE(xdata_addr);
    instr[2] = LOBYTE(xdata_addr);
    debug_command(CMD_DEBUG_INSTR_3B, instr, 3);
    for (i = 0; i < num_bytes; i++)
    {
        instr[0] = 0xE0;
        values[i] = debug_command(CMD_DEBUG_INSTR_1B, instr, 1);
        instr[0] = 0xA3;
        debug_command(CMD_DEBUG_INSTR_1B, instr, 1);
    }
}

void write_flash_memory_block(unsigned char *src, unsigned long start_addr,
                              unsigned short num_bytes)
{
    write_xdata_memory_block(ADDR_DMA_DESC_0, dma_desc_0, 8);
    write_xdata_memory_block(ADDR_DMA_DESC_1, dma_desc_1, 8);
    unsigned char len[2] = {HIBYTE(num_bytes), LOBYTE(num_bytes)};
    write_xdata_memory_block((ADDR_DMA_DESC_0+4), len, 2);
    write_xdata_memory_block((ADDR_DMA_DESC_1+4), len, 2);
    write_xdata_memory(DUP_DMA0CFGH, HIBYTE(ADDR_DMA_DESC_0));
    write_xdata_memory(DUP_DMA0CFGL, LOBYTE(ADDR_DMA_DESC_0));
    write_xdata_memory(DUP_DMA1CFGH, HIBYTE(ADDR_DMA_DESC_1));
    write_xdata_memory(DUP_DMA1CFGL, LOBYTE(ADDR_DMA_DESC_1));
    write_xdata_memory(DUP_FADDRH, HIBYTE(start_addr));
    write_xdata_memory(DUP_FADDRL, LOBYTE(start_addr));
    write_xdata_memory(DUP_DMAARM, CH_DBG_TO_BUF0);
    burst_write_block(src, num_bytes);
    write_xdata_memory(DUP_DMAARM, CH_BUF0_TO_FLASH);
    write_xdata_memory(DUP_FCTL, 0x0A);
    while (read_xdata_memory(DUP_FCTL) & 0x80);
}

void RunDUP(void)
{
    digitalWrite(DD, LOW);
    digitalWrite(DC, LOW);
    digitalWrite(RESET, LOW);
    delay(10);
    digitalWrite(RESET, HIGH);
    delay(10);
}

void ProgrammerInit(void)
{
    pinMode(DD, OUTPUT);
    pinMode(DC, OUTPUT);
    pinMode(RESET, OUTPUT);
    pinMode(LED, OUTPUT);
    digitalWrite(DD, LOW);
    digitalWrite(DC, LOW);
    digitalWrite(RESET, HIGH);
    digitalWrite(LED, LOW);
}

/******************************************************************************
 * WebUI 扩展：状态机、HTTP、SSE、LittleFS（无外部库依赖）
 *****************************************************************************/

// 状态机
enum CCLoaderState {
  STATE_IDLE,
  STATE_BURNING,
  STATE_MONITORING,
  STATE_SNIFFING
};
CCLoaderState g_state = STATE_IDLE;

// 全局对象：HTTP 服务器（80），SSE 服务器（81）
WebServer server(80);
WiFiServer sseServer(81);
HTTPUpdateServer httpUpdater;  // OTA 升级服务（/update）
#define SSE_MAX_CLIENTS 4
WiFiClient sseClients[SSE_MAX_CLIENTS];
bool sseActive[SSE_MAX_CLIENTS] = {false, false, false, false};

// 配置
struct Config {
  String wifi_ssid;
  String wifi_password;
  String device_name;  // 设备名称（浏览器标签显示）
  uint8_t verify;
};
Config g_config;

// 烧录状态
struct BurnState {
  uint32_t total_blocks;
  uint32_t current_block;
  uint8_t percent;
  String error;
  String info;
  bool done;
};
BurnState g_burn;

// 监控缓冲：256 字节攒一批推送，降低 SSE 频率，减少堆压力
#define MONITOR_BUF_SIZE 256
uint8_t g_monitor_buf[MONITOR_BUF_SIZE];
uint16_t g_monitor_len = 0;
uint32_t g_monitor_bytes_total = 0;
uint32_t g_monitor_baud = 115200;
unsigned long g_monitor_last_push = 0;

// 上传文件名（WRITE 阶段需要复用，不能用局部变量）
String g_upload_filename;
File g_upload_file;
bool g_upload_error = false;
bool g_upload_rejected_hex = false;  // API 上传 .hex 时拒绝（浏览器端会自动 hex2bin，API 不会）

// 配网模式标志：AP 模式下为 true，captive portal 启用
bool g_in_config_mode = false;

// LittleFS 挂载状态：false 时所有文件操作跳过，避免崩溃
// ESP32 首次烧录后 LittleFS 分区可能未格式化，begin() 失败时尝试 format
bool g_littlefs_ok = false;

// 设备唯一标识（基于 MAC 后 3 字节 hex，如 "A1B2C3"）
// 用于 AP 名称和主机名，避免多台设备冲突
String g_device_uid;
String g_default_ap_name;    // "CCLoader-Setup-A1B2C3"
String g_default_hostname;   // "CCLoader-A1B2C3"

// 异步烧录：POST /api/burn?async=1 立即返回，烧录在 loop() 中执行
bool g_burn_pending = false;
String g_burn_pending_filename;
bool g_burn_pending_verify = false;
uint32_t g_burn_task_id = 0;  // 单调递增的 task_id

// 清除配网：POST /api/nvreset 立即返回，在 loop() 中执行
bool g_nvreset_pending = false;

// 备份固件：POST /api/backup 立即返回，在 loop() 中执行
bool g_backup_pending = false;

// 监控日志环形缓冲：支持 Agent 轮询 GET /api/monitor/buffer?since=N
// 缓存最近 32KB 日志，Agent 可断点续传获取
// ESP32-SOLO-1 320KB DRAM 充足，从 ESP8266 时代 8KB 扩容到 32KB
#define MONITOR_RING_SIZE 32768
uint8_t g_monitor_ring[MONITOR_RING_SIZE];
uint32_t g_monitor_ring_head = 0;   // 下一个写入位置 (mod SIZE)
uint32_t g_monitor_ring_total = 0;  // 累计写入字节数（单调递增，不取模）

// ===== Sniffer 抓包功能（SNIFFING 状态）=====
// ZBOSS sniffer 固件通过 UART0 (P0_3, 115200 8N1) 输出 IEEE 802.15.4 帧
// ESP32-SOLO-1 Serial2 (IO16 RX) 接收，透传到 PC 端生成 pcap
// 详见 CCLoader_Sniffer抓包改造需求.md
// 64KB 环形缓冲：ESP32-SOLO-1 320KB DRAM 充足，从 ESP8266 时代 16KB 扩容到 64KB
// 动态分配（IDLE 时不占堆），缓冲满时丢弃最旧数据并插入丢包标记
#define SNIFFER_BUFFER_SIZE 65536
uint8_t* g_sniffer_buf = nullptr;
volatile uint32_t g_sniffer_head = 0;   // 写入位置 (Serial2 → 缓冲)
volatile uint32_t g_sniffer_tail = 0;   // 读取位置 (缓冲 → HTTP stream)
volatile uint32_t g_sniffer_total_rx = 0;
volatile uint32_t g_sniffer_total_sent = 0;
volatile uint32_t g_sniffer_drop_bytes = 0;  // 缓冲满累计丢弃字节数
volatile uint32_t g_sniffer_drop_count = 0;  // 丢弃次数（每 1024 字节计一次）
volatile uint32_t g_sniffer_drop_accum = 0;  // 丢弃累计（满 1024 推送标记包）
uint8_t g_sniffer_channel = 11;
uint32_t g_sniffer_baud = 115200;
unsigned long g_sniffer_start_ms = 0;
// sniffer stream 客户端（同时只允许 1 个，避免多客户端分流数据）
WiFiClient g_sniffer_client;
bool g_sniffer_client_active = false;

// ===== 简易 JSON 工具（仅处理顶层简单 key:value，避免 ArduinoJson 依赖）=====
// JSON 字符串内的转义：把 " 和 \ 反转义，避免破坏 JSON
String jsonEscape(const String& s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else out += c;
  }
  return out;
}

// 在 JSON 文本中查找 "key":"value" 并返回 value（去除引号和转义）
String jsonGetString(const String& json, const String& key) {
  String needle = "\"" + key + "\"";
  int p = json.indexOf(needle);
  if (p < 0) return "";
  p = json.indexOf(':', p + needle.length());
  if (p < 0) return "";
  p++;
  // 跳过空白
  while (p < (int)json.length() && (json[p] == ' ' || json[p] == '\t')) p++;
  if (p >= (int)json.length() || json[p] != '"') return "";
  p++;
  String out;
  while (p < (int)json.length()) {
    char c = json[p++];
    if (c == '\\' && p < (int)json.length()) {
      char n = json[p++];
      if (n == 'n') out += '\n';
      else if (n == 'r') out += '\r';
      else if (n == 't') out += '\t';
      else out += n;
    } else if (c == '"') {
      break;
    } else {
      out += c;
    }
  }
  return out;
}

// 在 JSON 文本中查找 "key":bool 并返回布尔值
bool jsonGetBool(const String& json, const String& key, bool def) {
  String needle = "\"" + key + "\"";
  int p = json.indexOf(needle);
  if (p < 0) return def;
  p = json.indexOf(':', p + needle.length());
  if (p < 0) return def;
  p++;
  while (p < (int)json.length() && (json[p] == ' ' || json[p] == '\t')) p++;
  if (json.substring(p, p + 4) == "true") return true;
  if (json.substring(p, p + 5) == "false") return false;
  return def;
}

// 在 JSON 文本中查找 "key":number 并返回整数
long jsonGetInt(const String& json, const String& key, long def) {
  String needle = "\"" + key + "\"";
  int p = json.indexOf(needle);
  if (p < 0) return def;
  p = json.indexOf(':', p + needle.length());
  if (p < 0) return def;
  p++;
  while (p < (int)json.length() && (json[p] == ' ' || json[p] == '\t')) p++;
  String num;
  while (p < (int)json.length()) {
    char c = json[p];
    if ((c >= '0' && c <= '9') || c == '-' || c == '+') {
      num += c;
      p++;
    } else break;
  }
  if (num.length() == 0) return def;
  return num.toInt();
}

// URL 解码（DELETE 路径中文件名可能含特殊字符）
String urlDecode(const String& s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '%' && i + 2 < s.length()) {
      char h1 = s[i + 1];
      char h2 = s[i + 2];
      auto hexVal = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
      };
      int v = hexVal(h1) * 16 + hexVal(h2);
      if (v >= 0) { out += (char)v; i += 2; continue; }
    }
    if (c == '+') out += ' ';
    else out += c;
  }
  return out;
}

// ===== 配置文件读写（手工 JSON）=====
void loadConfig() {
  g_config.wifi_ssid = "";
  g_config.wifi_password = "";
  g_config.device_name = "";
  g_config.verify = 0;

  if (!g_littlefs_ok) return;  // LittleFS 未挂载，使用默认空配置
  if (LittleFS.exists("/config.json")) {
    File f = LittleFS.open("/config.json", "r");
    if (f) {
      String s;
      while (f.available() && s.length() < 1024) s += (char)f.read();
      f.close();
      g_config.wifi_ssid = jsonGetString(s, "wifi_ssid");
      g_config.wifi_password = jsonGetString(s, "wifi_password");
      g_config.device_name = jsonGetString(s, "device_name");
      g_config.verify = jsonGetBool(s, "verify", false) ? 1 : 0;
    }
  }
}

void saveConfig(const String& ssid, const String& pwd, const String& deviceName, uint8_t verify) {
  if (!g_littlefs_ok) return;  // LittleFS 未挂载，跳过保存
  String json = "{\n";
  json += "  \"wifi_ssid\": \"" + jsonEscape(ssid) + "\",\n";
  json += "  \"wifi_password\": \"" + jsonEscape(pwd) + "\",\n";
  json += "  \"device_name\": \"" + jsonEscape(deviceName) + "\",\n";
  json += "  \"verify\": " + String(verify ? "true" : "false") + "\n";
  json += "}\n";
  File f = LittleFS.open("/config.json", "w");
  if (f) {
    f.print(json);
    f.close();
  }
}

// ===== WiFi =====
// 生成设备唯一标识：基于 MAC 后 3 字节 hex（如 "A1B2C3"）
// 多台 CCLoader 共存时避免 AP 名称和主机名冲突
// 注意：必须在 WiFi.mode() 之后调用，否则 MAC 未初始化返回 FFFFFF
void initDeviceUid() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char uid[8];
  snprintf(uid, sizeof(uid), "%02X%02X%02X", mac[3], mac[4], mac[5]);
  g_device_uid = String(uid);
  g_default_ap_name = "CCLoader-" + g_device_uid;
  g_default_hostname = "CCLoader-" + g_device_uid;
  Serial.printf("Device UID: %s, AP: %s, Hostname: %s, MAC: %s\n",
                g_device_uid.c_str(), g_default_ap_name.c_str(),
                g_default_hostname.c_str(), WiFi.macAddress().c_str());
}

// 配置 WiFi 射频参数：最大发射功率 + 禁用节能模式
// ESP32 默认节能模式会导致 AP 连接不稳定（表现为连不上/频繁断开）
void applyWifiRadioParams() {
  WiFi.setSleep(false);                  // 禁用 Modem-sleep，提高稳定性
  WiFi.setTxPower(WIFI_POWER_19_5dBm);   // 最大发射功率（19.5 dBm）
}

// 进入配网模式：开放 AP + captive portal
// 使用 WIFI_AP_STA 模式：AP 持续在线，STA 可同时扫描网络
// （WIFI_AP 模式下 scanNetworks 会临时切 STA 导致 AP 断开，反复扫描会使 WiFi 状态混乱）
void enterConfigMode(const char* reason) {
  WiFi.mode(WIFI_AP_STA);
  applyWifiRadioParams();
  // 信道 1，开放 AP，最大连接数 4
  WiFi.softAP(g_default_ap_name.c_str(), NULL, 1, 0, 4);
  g_in_config_mode = true;
  Serial.printf("Config mode (%s): AP '%s' open, IP: ",
                reason, g_default_ap_name.c_str());
  Serial.println(WiFi.softAPIP());
}

void initWiFi() {
  // 先设置 AP_STA 模式，确保 MAC 就绪（mode 之前 macAddress 返回 FFFFFF）
  WiFi.mode(WIFI_AP_STA);
  delay(10);
  initDeviceUid();  // MAC 已就绪，生成设备 UID

  if (g_config.wifi_ssid.length() == 0) {
    // 无配置，进入配网模式（AP_STA，AP 可用 + STA 可扫描）
    enterConfigMode("no config");
    return;
  }
  // 有配置，关闭 AP，切到纯 STA 模式连接
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(g_default_hostname.c_str());  // 设置主机名（STA 模式生效）
  applyWifiRadioParams();
  WiFi.begin(g_config.wifi_ssid, g_config.wifi_password);
  Serial.printf("Connecting to %s", g_config.wifi_ssid.c_str());
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("Connected, IP: ");
    Serial.println(WiFi.localIP());
    g_in_config_mode = false;
  } else {
    Serial.println("\nWiFi connect failed, switching to config mode");
    enterConfigMode("connect failed");
  }
}

// 切换到 STA 模式（配网成功后调用）
bool switchToStaMode(const String& ssid, const String& pwd) {
  Serial.printf("Trying connect to %s ...\n", ssid.c_str());
  // 临时切换 STA 连接测试
  WiFi.softAPdisconnect(true);  // 关闭 AP
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(g_default_hostname.c_str());
  applyWifiRadioParams();
  WiFi.begin(ssid, pwd);
  // 同步等待 8 秒
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
    delay(200);
    server.handleClient();  // 保持 HTTP 响应能力
    sseLoop();
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected, IP: ");
    Serial.println(WiFi.localIP());
    g_in_config_mode = false;
    // 配网成功后启动 NTP 授时（北京时间 UTC+8）
    configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org", "time.windows.com");
    Serial.println("NTP configured (CST-8)");
    // 保存配置
    saveConfig(ssid, pwd, g_config.device_name, g_config.verify);
    g_config.wifi_ssid = ssid;
    g_config.wifi_password = pwd;
    return true;
  }
  // 连接失败，回到 AP 配网模式（AP_STA，支持扫描）
  Serial.println("Connect failed, back to config mode");
  WiFi.disconnect();
  enterConfigMode("connect failed");
  return false;
}

// ===== Base64 编码（监控数据推送用）=====
String base64Encode(const uint8_t *data, size_t len) {
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String result;
  result.reserve((len + 2) / 3 * 4);
  for (size_t i = 0; i < len; i += 3) {
    uint32_t n = ((uint32_t)data[i]) << 16;
    if (i + 1 < len) n |= ((uint32_t)data[i+1]) << 8;
    if (i + 2 < len) n |= data[i+2];
    result += alphabet[(n >> 18) & 0x3F];
    result += alphabet[(n >> 12) & 0x3F];
    result += (i + 1 < len) ? alphabet[(n >> 6) & 0x3F] : '=';
    result += (i + 2 < len) ? alphabet[n & 0x3F] : '=';
  }
  return result;
}

// ===== SSE 客户端管理 =====
void sseLoop() {
  // 接受新连接
  WiFiClient c = sseServer.accept();
  if (c) {
    bool accepted = false;
    for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
      if (!sseActive[i] || !sseClients[i].connected()) {
        sseClients[i] = c;
        sseActive[i] = true;
        // 发送 SSE 响应头
        sseClients[i].println("HTTP/1.1 200 OK");
        sseClients[i].println("Content-Type: text/event-stream");
        sseClients[i].println("Cache-Control: no-cache");
        sseClients[i].println("Connection: keep-alive");
        sseClients[i].println("Access-Control-Allow-Origin: *");
        sseClients[i].println();
        // 发送初始状态
        const char* stateStr = "idle";
        if (g_state == STATE_BURNING) stateStr = "burning";
        else if (g_state == STATE_MONITORING) stateStr = "monitoring";
        else if (g_state == STATE_SNIFFING) stateStr = "sniffing";
        String json = "{\"type\":\"status\",\"state\":\"";
        json += stateStr;
        json += "\"}";
        sseClients[i].printf("event: message\ndata: %s\n\n", json.c_str());
        accepted = true;
        Serial.printf("SSE client #%d connected\n", i);
        break;
      }
    }
    if (!accepted) {
      // 客户端满，关闭新连接
      c.stop();
    }
  }
  // 清理断连
  for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
    if (sseActive[i] && !sseClients[i].connected()) {
      sseClients[i].stop();
      sseActive[i] = false;
    }
  }
}

// 向所有已连接的 SSE 客户端推送一条 event:data
// 可靠写入：带重试机制，避免 WiFi 缓冲区满导致事件丢失
void sseSend(const String& json) {
  String msg = "event: message\ndata: " + json + "\n\n";
  const char* data = msg.c_str();
  size_t len = msg.length();
  for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
    if (sseActive[i] && sseClients[i].connected()) {
      size_t written = 0;
      unsigned long start = millis();
      while (written < len && millis() - start < 500 && sseClients[i].connected()) {
        int w = sseClients[i].write(data + written, len - written);
        if (w > 0) {
          written += w;
        } else {
          delay(1);
        }
        yield();
      }
      if (written < len) {
        // 写入失败，连接可能已断开，清理
        Serial.printf("[SSE] client #%d write failed (%u/%u), disconnecting\n",
                      i, (unsigned)written, (unsigned)len);
        sseClients[i].stop();
        sseActive[i] = false;
      }
    }
  }
}

// ===== SSE 推送：烧录进度 / 监控数据 =====
void pushBurnProgress() {
  String json = "{\"type\":\"burn_progress\"";
  json += ",\"percent\":" + String(g_burn.percent);
  json += ",\"current_block\":" + String(g_burn.current_block);
  json += ",\"total_blocks\":" + String(g_burn.total_blocks);
  json += ",\"done\":" + String(g_burn.done ? "true" : "false");
  json += ",\"error\":\"" + jsonEscape(g_burn.error) + "\"";
  json += ",\"info\":\"" + jsonEscape(g_burn.info) + "\"";
  json += "}";
  sseSend(json);
}

void pushMonitorData() {
  if (g_monitor_len == 0) return;
  // 写入环形缓冲（供 Agent 轮询 /api/monitor/buffer 获取）
  for (uint16_t i = 0; i < g_monitor_len; i++) {
    g_monitor_ring[g_monitor_ring_head] = g_monitor_buf[i];
    g_monitor_ring_head = (g_monitor_ring_head + 1) % MONITOR_RING_SIZE;
    g_monitor_ring_total++;
  }
  // SSE 推送给在线浏览器
  String b64 = base64Encode(g_monitor_buf, g_monitor_len);
  String json = "{\"type\":\"monitor_data\",\"data\":\"" + b64 + "\"}";
  sseSend(json);
  g_monitor_len = 0;
}

// ===== 烧录：从 LittleFS 读取 BIN 烧录到 CC2530 =====
void burnFromLittleFS(const String& filename, bool verify) {
  File f = LittleFS.open("/" + filename, "r");
  if (!f) {
    g_burn.error = "file not found: " + filename;
    g_burn.done = true;
    pushBurnProgress();
    return;
  }

  uint32_t fileSize = f.size();
  uint32_t totalBlocks = (fileSize + 511) / 512;
  g_burn.total_blocks = totalBlocks;
  g_burn.current_block = 0;
  g_burn.percent = 0;
  g_burn.done = false;
  g_burn.error = "";

  debug_init();
  uint8_t chip_id = read_chip_id();
  if (chip_id == 0) {
    g_burn.error = "chip not detected";
    g_burn.done = true;
    pushBurnProgress();
    f.close();
    return;
  }

  RunDUP();
  debug_init();
  chip_erase();
  RunDUP();
  debug_init();

  // 切换到外部晶振
  write_xdata_memory(DUP_CLKCONCMD, 0x80);
  unsigned long xosc_start = millis();
  while (read_xdata_memory(DUP_CLKCONSTA) != 0x80) {
    if (millis() - xosc_start > 2000) {
      g_burn.error = "XOSC timeout";
      g_burn.done = true;
      pushBurnProgress();
      f.close();
      return;
    }
  }

  uint8_t debug_config = 0x22;
  debug_command(CMD_WR_CONFIG, &debug_config, 1);

  uint32_t addr = 0;
  uint8_t buf[512];
  uint32_t blockIndex = 0;

  while (f.available()) {
    size_t got = f.read(buf, 512);
    if (got < 512) {
      memset(buf + got, 0xFF, 512 - got);
    }

    write_flash_memory_block(buf, addr, 512);

    if (verify) {
      uint8_t bank = addr / (512 * 16);
      uint16_t offset = (addr % (512 * 16)) * 4;
      uint8_t read_data[512];
      read_flash_memory_block(bank, offset, 512, read_data);
      for (int i = 0; i < 512; i++) {
        if (read_data[i] != buf[i]) {
          g_burn.error = "verify failed at block " + String(blockIndex);
          g_burn.done = true;
          pushBurnProgress();
          f.close();
          RunDUP();
          return;
        }
      }
    }

    addr += 128;
    blockIndex++;
    g_burn.current_block = blockIndex;
    g_burn.percent = (blockIndex * 100) / g_burn.total_blocks;

    // 每 16 块推送一次进度 + 处理 WiFi（约 32 次/256KB）
    // 原 32 块间隔在偶发慢块时可能踩 ESP8266 软件 WDT (~3.2s) 边界，
    // ESP32-SOLO-1 无软件 WDT 但保留 16 块节奏，进度条每 3% 更新一次更流畅
    if (blockIndex % 16 == 0 || blockIndex == g_burn.total_blocks) {
      pushBurnProgress();
      sseLoop();
      server.handleClient();
      yield();  // 显式喂狗 + 让 WiFi 任务调度
    }
  }

  f.close();
  RunDUP();
  g_burn.done = true;
  pushBurnProgress();
}

// ===== 清除 CC2530 配网信息（保留固件，仅擦除 NV 区域） =====
// 流程：读取整个 Flash → 清除尾部 NV 区域 → 全片擦除 → 写回
// 本质上是"读-改-写"：读回固件，清掉配网数据，再重新烧录
void nvResetCC2530() {
  const char* tmpFile = "/nv_reset.bin";
  const uint32_t flashSize = 256 * 1024;   // 256KB (CC2530F256)
  const uint32_t blockSize = 512;
  const uint32_t nvClearBytes = 4 * 1024;   // 4KB (NV 区域)
  const uint32_t totalBlocks = flashSize / blockSize;

  g_burn.total_blocks = totalBlocks;
  g_burn.current_block = 0;
  g_burn.percent = 0;
  g_burn.done = false;
  g_burn.error = "";

  // 删除上次残留的临时文件
  if (LittleFS.exists(tmpFile)) LittleFS.remove(tmpFile);

  // Phase 1: 读取 CC2530 全部 Flash 到 LittleFS 临时文件
  File f = LittleFS.open(tmpFile, "w");
  if (!f) {
    g_burn.error = "cannot create temp file";
    g_burn.done = true;
    pushBurnProgress();
    return;
  }

  // 进入 debug 模式
  digitalWrite(LED, HIGH);
  debug_init();
  if (read_chip_id() == 0) {
    g_burn.error = "chip not detected";
    g_burn.done = true;
    pushBurnProgress();
    f.close();
    LittleFS.remove(tmpFile);
    digitalWrite(LED, LOW);
    return;
  }
  RunDUP();
  debug_init();

  // 逐块读取 Flash
  uint8_t buf[512];
  for (uint32_t i = 0; i < totalBlocks; i++) {
    uint32_t addr = i * 128;
    uint8_t bank = addr / (512 * 16);
    uint16_t offset = (addr % (512 * 16)) * 4;
    read_flash_memory_block(bank, offset, 512, buf);
    f.write(buf, 512);

    g_burn.current_block = i + 1;
    g_burn.percent = (i + 1) * 50 / totalBlocks;  // 读取阶段占 0-50%
    if (i % 16 == 0 || i == totalBlocks - 1) {
      pushBurnProgress();
      sseLoop();
      server.handleClient();
      yield();
    }
  }
  f.close();

  // Phase 2: 清除 NV 区域（最后 4KB 填充 0xFF）
  f = LittleFS.open(tmpFile, "r+");
  if (!f) {
    g_burn.error = "cannot modify temp file";
    g_burn.done = true;
    pushBurnProgress();
    LittleFS.remove(tmpFile);
    digitalWrite(LED, LOW);
    return;
  }
  uint32_t nvOffset = flashSize - nvClearBytes;
  if (!f.seek(nvOffset)) {
    g_burn.error = "seek failed";
    g_burn.done = true;
    pushBurnProgress();
    f.close();
    LittleFS.remove(tmpFile);
    digitalWrite(LED, LOW);
    return;
  }
  uint8_t ff[512];
  memset(ff, 0xFF, 512);
  for (uint32_t i = 0; i < nvClearBytes / blockSize; i++) {
    f.write(ff, 512);
  }
  f.close();

  // Phase 3: 烧录回 CC2530（全片擦除 + 写入 + 校验 + RunDUP）
  // burnFromLittleFS 内部会重新 debug_init、chip_erase、写回、RunDUP
  // 进度从 0-100% 显示，与读取阶段连贯
  g_burn.percent = 50;
  pushBurnProgress();
  burnFromLittleFS(tmpFile, true);

  // 清理临时文件
  LittleFS.remove(tmpFile);
  digitalWrite(LED, LOW);
}

// ===== 备份 CC2530 固件：读取 Flash 保存到 LittleFS =====
// 生成带时间戳的文件名 backup_YYYYMMDD_HHMMSS.bin
// 完成后出现在文件列表中，可下载到本地
void backupCC2530() {
  const uint32_t flashSize = 256 * 1024;
  const uint32_t blockSize = 512;
  const uint32_t totalBlocks = flashSize / blockSize;

  // 生成时间戳文件名
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  char filename[64];
  if (t->tm_year > 70) {
    snprintf(filename, sizeof(filename), "backup_%04d%02d%02d_%02d%02d%02d.bin",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);
  } else {
    // NTP 未授时，用毫秒时间戳避免重名
    snprintf(filename, sizeof(filename), "backup_%lu.bin", (unsigned long)millis());
  }

  g_burn.total_blocks = totalBlocks;
  g_burn.current_block = 0;
  g_burn.percent = 0;
  g_burn.done = false;
  g_burn.error = "";

  // 进入 debug 模式
  digitalWrite(LED, HIGH);
  debug_init();
  if (read_chip_id() == 0) {
    g_burn.error = "chip not detected";
    g_burn.done = true;
    pushBurnProgress();
    digitalWrite(LED, LOW);
    return;
  }
  RunDUP();
  debug_init();

  // 创建输出文件
  File f = LittleFS.open("/" + String(filename), "w");
  if (!f) {
    g_burn.error = "cannot create file";
    g_burn.done = true;
    pushBurnProgress();
    digitalWrite(LED, LOW);
    return;
  }

  // 逐块读取 Flash 并写入文件
  uint8_t buf[512];
  uint32_t lastPercent = 0;
  for (uint32_t i = 0; i < totalBlocks; i++) {
    uint32_t addr = i * 128;
    uint8_t bank = addr / (512 * 16);
    uint16_t offset = (addr % (512 * 16)) * 4;
    read_flash_memory_block(bank, offset, 512, buf);
    f.write(buf, 512);

    g_burn.current_block = i + 1;
    g_burn.percent = (i + 1) * 100 / totalBlocks;
    if (g_burn.percent != lastPercent) {
      lastPercent = g_burn.percent;
      pushBurnProgress();
      sseLoop();
      server.handleClient();
      yield();
    }
  }
  f.close();
  RunDUP();

  g_burn.done = true;
  g_burn.error = "";
  g_burn.info = "备份完成: " + String(filename);
  pushBurnProgress();
  Serial.printf("Backup saved: %s (%u bytes)\n", filename, flashSize);
  digitalWrite(LED, LOW);
}

// ===== CC2530 复位（通过 GPIO5/RESETn 控制，无需手动按按钮）=====
// 通用串口监控场景：只操作 RESETn 一根线
// 拉低 DD/DC 是调试专用副作用，会让 CC Debug 接口进入非标准电平，
// 对运行中的应用固件有干扰，故移除
void resetCC2530() {
  // CC2530 RESETn 引脚时序：低电平≥10μs 复位，上升沿启动
  digitalWrite(RESET, LOW);
  delay(10);
  digitalWrite(RESET, HIGH);
  delay(50);  // 等待 CC2530 晶振稳定和固件启动
}

// ===== 监控模式：Serial2 接收 CC2530 日志并推送 =====
// autoReset=true 时进入监控前自动复位 CC2530，可捕获 main() 启动日志
void enterMonitorMode(uint32_t baud, bool autoReset) {
  // 先复位 CC2530（在 Serial2 切换前，确保从启动日志开始捕获）
  if (autoReset) {
    resetCC2530();
  }
  // 切换 Serial2 到 CC2530 波特率，IO16 接收 P0_3 日志
  // 使用 updateBaudRate 避免 end()/begin() 释放 UART 驱动导致数据丢失
  Serial2.flush();
  Serial2.updateBaudRate(baud);
  g_monitor_baud = baud;
  g_monitor_len = 0;
  g_monitor_bytes_total = 0;
  g_monitor_ring_head = 0;
  g_monitor_ring_total = 0;
  g_monitor_last_push = millis();
  g_state = STATE_MONITORING;

  // 通知前端
  String json = "{\"type\":\"monitor_start\",\"baud\":" + String(baud) + "}";
  sseSend(json);
  Serial.printf("Monitor started: baud=%u\n", baud);
}

void exitMonitorMode() {
  // 推送剩余数据
  if (g_monitor_len > 0) pushMonitorData();
  // 恢复 Serial2 到默认波特率
  Serial2.flush();
  Serial2.updateBaudRate(115200);
  g_state = STATE_IDLE;
  g_monitor_len = 0;

  String json = "{\"type\":\"monitor_stop\"}";
  sseSend(json);
  Serial.println("Monitor stopped");
}

// ===== Sniffer 抓包模式（SNIFFING 状态）=====
// ZBOSS sniffer 固件经 UART0 (P0_3, 115200 8N1) 输出 IEEE 802.15.4 帧
// ESP32-SOLO-1 Serial2 (IO16 RX) 接收原始字节存入环形缓冲，
// 由 /api/sniffer/stream 通过 HTTP chunked 透传到 PC，PC 端解析生成 pcap
// 详见 CCLoader_Sniffer抓包改造需求.md

// 写入环形缓冲（loop 中从 Serial2 读取后调用）
// 缓冲满时丢弃最旧数据并累计 drop_bytes，每满 1024 字节插入一个丢包标记包
void snifferWrite(const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    uint32_t next = (g_sniffer_head + 1) % SNIFFER_BUFFER_SIZE;
    if (next == g_sniffer_tail) {
      // 缓冲满，丢弃最旧数据
      g_sniffer_tail = (g_sniffer_tail + 1) % SNIFFER_BUFFER_SIZE;
      g_sniffer_drop_bytes = g_sniffer_drop_bytes + 1;
      g_sniffer_drop_accum = g_sniffer_drop_accum + 1;
      // 累计 1024 字节丢弃，插入标记包（ZBOSS 头格式，type=0xFF）
      if (g_sniffer_drop_accum >= 1024) {
        g_sniffer_drop_accum = 0;
        g_sniffer_drop_count = g_sniffer_drop_count + 1;
        // 标记包：len=8, type=0xFF, tail=0x0000, dropped_bytes(大端 4 字节)
        uint8_t marker[8];
        marker[0] = 8;
        marker[1] = 0xFF;
        marker[2] = 0x00;
        marker[3] = 0x00;
        uint32_t db = g_sniffer_drop_bytes;
        marker[4] = (db >> 24) & 0xFF;
        marker[5] = (db >> 16) & 0xFF;
        marker[6] = (db >> 8) & 0xFF;
        marker[7] = db & 0xFF;
        // 递归写入标记包（此时已腾出空间，不会再次溢出）
        snifferWrite(marker, 8);
      }
    }
    g_sniffer_buf[g_sniffer_head] = data[i];
    g_sniffer_head = next;
    g_sniffer_total_rx = g_sniffer_total_rx + 1;
  }
}

// 从环形缓冲读取到 out，最多 max_len 字节，返回实际读取数
size_t snifferRead(uint8_t *out, size_t max_len) {
  size_t count = 0;
  while (count < max_len && g_sniffer_tail != g_sniffer_head) {
    out[count++] = g_sniffer_buf[g_sniffer_tail];
    g_sniffer_tail = (g_sniffer_tail + 1) % SNIFFER_BUFFER_SIZE;
  }
  return count;
}

// 进入 sniffer 模式
// sniffer 固件上电默认启动通道 11（zb_sniffer_init 中 start_channel=11），
// 启动时不发送通道号（避免 Serial2.write 影响 RX），如需切换通道用 /api/sniffer/channel。
bool enterSnifferMode(uint8_t channel, uint32_t baud) {
  Serial.printf("[SNIFFER] enterSnifferMode: ch=%u baud=%u\n", channel, baud);
  // 动态分配 sniffer 缓冲（IDLE 时不占用堆，给上传/烧录留内存）
  if (!g_sniffer_buf) {
    g_sniffer_buf = (uint8_t*)malloc(SNIFFER_BUFFER_SIZE);
    if (!g_sniffer_buf) {
      Serial.println("[SNIFFER] ERROR: malloc failed for sniffer buffer");
      return false;  // 内存不足
    }
    Serial.printf("[SNIFFER] Allocated buffer: %u bytes\n", SNIFFER_BUFFER_SIZE);
  }
  // 先复位 CC2530（在 Serial2 切换前，确保 sniffer 固件从干净状态启动）
  // 实测：不加复位时 Serial2.available() 始终返回 0（与 monitor 模式的关键差异）
  Serial.println("[SNIFFER] Resetting CC2530...");
  resetCC2530();
  // 如果波特率与 setup 初始化的 115200 不同，用 updateBaudRate 切换
  // 使用 updateBaudRate 避免 end()/begin() 释放 UART 驱动导致数据丢失
  if (baud != 115200) {
    Serial2.flush();
    Serial2.updateBaudRate(baud);
  }
  Serial.printf("[SNIFFER] Serial2 ready: RX=IO%d, baud=%u\n", CC_SERIAL_RX, baud);
  // 等待 sniffer 固件初始化完成（zb_sniffer_init 启动默认通道 11）
  Serial.println("[SNIFFER] Waiting 200ms for sniffer firmware init...");
  delay(200);
  // 清空 RX 缓冲中 sniffer 启动期间的日志字节（限时 50ms，避免清空有效数据）
  int cleared = 0;
  unsigned long clearStart = millis();
  while (Serial2.available() && millis() - clearStart < 50) {
    Serial2.read();
    cleared++;
  }
  Serial.printf("[SNIFFER] Cleared %d startup bytes from RX buffer\n", cleared);
  // 清空环形缓冲与统计
  g_sniffer_head = 0;
  g_sniffer_tail = 0;
  g_sniffer_total_rx = 0;
  g_sniffer_total_sent = 0;
  g_sniffer_drop_bytes = 0;
  g_sniffer_drop_count = 0;
  g_sniffer_drop_accum = 0;
  // 注意：channel 参数仅记录用户请求的通道，实际抓包通道由 sniffer 固件决定
  // 若 channel != 11，需在启动后调用 /api/sniffer/channel 切换
  g_sniffer_channel = channel;
  g_sniffer_baud = baud;
  g_sniffer_start_ms = millis();
  g_sniffer_client_active = false;
  g_state = STATE_SNIFFING;
  digitalWrite(LED, HIGH);
  // 通知前端
  String json = "{\"type\":\"sniffer_start\",\"channel\":" + String(channel) + "}";
  sseSend(json);
  Serial.println("[SNIFFER] sniffer_start event sent via SSE");
  // 若请求通道 ≠ 默认 11，自动发送 1 字节通道号切换
  // ESP32 双串口架构：Serial2 写入不影响 Serial2 RX（与 ESP8266 单串口不同）
  if (channel != 11) {
    delay(100);  // 短暂延迟确保 sniffer 已就绪
    Serial2.write(channel);
    Serial2.flush();
    delay(200);  // 等待 sniffer 切换通道并清空 RF FIFO
    unsigned long chClearStart = millis();
    while (Serial2.available() && millis() - chClearStart < 50) {
      Serial2.read();
    }
    g_sniffer_head = 0;  // 清空缓冲中切换前接收的数据
    g_sniffer_tail = 0;
  }
  Serial.printf("[SNIFFER] Started: channel=%u baud=%u\n", channel, baud);
  return true;
}

// 退出 sniffer 模式
void exitSnifferMode() {
  g_state = STATE_IDLE;
  // 断开 stream 客户端
  if (g_sniffer_client_active && g_sniffer_client.connected()) {
    g_sniffer_client.stop();
  }
  g_sniffer_client_active = false;
  digitalWrite(LED, LOW);
  // 释放 sniffer 缓冲（归还给堆，供上传/烧录使用）
  if (g_sniffer_buf) {
    free(g_sniffer_buf);
    g_sniffer_buf = nullptr;
  }
  // 恢复 Serial2 到默认波特率
  Serial2.flush();
  Serial2.updateBaudRate(115200);
  String json = "{\"type\":\"sniffer_stop\"}";
  sseSend(json);
  Serial.println("Sniffer stopped");
}

// loop 中的 sniffer 处理：从 Serial2 读取数据写入环形缓冲
// 采用与 handleMonitoring 完全一致的逐字节读取方式（Serial2.readBytes 可能因 timedRead
// 阻塞或行为异常导致收不到数据，逐字节 read 已在 monitor 模式验证可靠）
void handleSniffing() {
  const uint16_t MAX_READ_PER_LOOP = 512;
  uint16_t readThisCall = 0;
  while (Serial2.available() && readThisCall < MAX_READ_PER_LOOP) {
    uint8_t ch = Serial2.read();
    // 写入环形缓冲
    uint32_t next = (g_sniffer_head + 1) % SNIFFER_BUFFER_SIZE;
    if (next == g_sniffer_tail) {
      // 缓冲满，丢弃最旧数据
      g_sniffer_tail = (g_sniffer_tail + 1) % SNIFFER_BUFFER_SIZE;
      g_sniffer_drop_bytes = g_sniffer_drop_bytes + 1;
      g_sniffer_drop_accum = g_sniffer_drop_accum + 1;
      if (g_sniffer_drop_accum >= 1024) {
        g_sniffer_drop_accum = 0;
        g_sniffer_drop_count = g_sniffer_drop_count + 1;
        // 插入丢包标记包（简化处理，直接覆盖）
        uint8_t marker[8] = {8, 0xFF, 0x00, 0x00,
                             (uint8_t)(g_sniffer_drop_bytes >> 24),
                             (uint8_t)(g_sniffer_drop_bytes >> 16),
                             (uint8_t)(g_sniffer_drop_bytes >> 8),
                             (uint8_t)(g_sniffer_drop_bytes & 0xFF)};
        for (uint8_t i = 0; i < 8; i++) {
          g_sniffer_buf[g_sniffer_head] = marker[i];
          g_sniffer_head = (g_sniffer_head + 1) % SNIFFER_BUFFER_SIZE;
          // 标记包写入时也可能触发丢弃，但 8 字节不会循环满
        }
        next = (g_sniffer_head + 1) % SNIFFER_BUFFER_SIZE;
      }
    }
    g_sniffer_buf[g_sniffer_head] = ch;
    g_sniffer_head = next;
    g_sniffer_total_rx = g_sniffer_total_rx + 1;
    readThisCall++;
  }
  // 调试日志：每秒打印一次接收统计（仅 SNIFFING 状态）
  static unsigned long lastDebugMs = 0;
  static unsigned long loopCount = 0;
  loopCount++;
  if (g_state == STATE_SNIFFING && millis() - lastDebugMs > 1000) {
    lastDebugMs = millis();
    Serial.printf("[SNIFFER] rx=%u sent=%u avail=%d buf=%u drop=%u wifi=%d rssi=%d heap=%u loop=%u\n",
                  (unsigned)g_sniffer_total_rx, (unsigned)g_sniffer_total_sent,
                  Serial2.available(),
                  (unsigned)((g_sniffer_head >= g_sniffer_tail) ?
                    (g_sniffer_head - g_sniffer_tail) :
                    (SNIFFER_BUFFER_SIZE - g_sniffer_tail + g_sniffer_head)),
                  (unsigned)g_sniffer_drop_bytes,
                  (int)WiFi.status(), (int)WiFi.RSSI(),
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)loopCount);
    loopCount = 0;
  }
}

void handleMonitoring() {
  // 限制单次 loop 读取字节数，避免 CC2530 高速输出时 while(Serial2.available())
  // 长时间占用 CPU，导致 server.handleClient() 被饿死、/api/status 等接口假死
  // 128 字节 ≈ 1ms @ 115200bps，足够让 loop() 每秒调度数百次 HTTP
  const uint16_t MAX_READ_PER_LOOP = 128;
  uint16_t readThisCall = 0;
  while (Serial2.available() && readThisCall < MAX_READ_PER_LOOP) {
    uint8_t ch = Serial2.read();
    if (g_monitor_len < MONITOR_BUF_SIZE) {
      g_monitor_buf[g_monitor_len++] = ch;
      g_monitor_bytes_total++;
    }
    readThisCall++;
  }
  // 攒够 256 字节或 200ms 静默才推送：
  // - 原 50ms 太频繁，每秒触发 ~20 次 String 拼接 + base64 + 多客户端 write
  // - 200ms 仍然在人类感知范围内，但堆压力降低 4 倍
  if (g_monitor_len >= MONITOR_BUF_SIZE ||
      (g_monitor_len > 0 && millis() - g_monitor_last_push > 200)) {
    pushMonitorData();
    g_monitor_last_push = millis();
  }
}

// ===== HTTP 路由 =====
// 优先使用固件内嵌资源（PROGMEM），OTA 升级时一并更新
// LittleFS 中的同名文件作为备份（旧固件兼容）

// 分块发送 PROGMEM 大响应（Content-Length + 分块 sendContent_P，每块 1000 字节）
// 解决 tailscale 等 PMTU < 1500 链路的 IP 分片丢包问题：
// ESP32 默认 TCP MSS=1460，单 TCP 段 IP 包 1500 字节，
// 经 tailscale (PMTU 1280) 转发时 DF=1 分片被丢弃，大响应截断。
// 每块 1000 字节 → 单 TCP 段 IP 包 ~1040 < 1280，无需分片即可通过。
// 禁用 Nagle（TCP_NODELAY）防止多个小块合并成超过 PMTU 的大包。
void sendChunked_P(const char* mime, PGM_P data, size_t len) {
  WiFiClient client = server.client();
  client.setNoDelay(true);   // 禁用 Nagle，每块独立发送
  client.setTimeout(5000);   // 增加写超时
  server.setContentLength(len);
  server.sendHeader("Connection", "close");
  server.send(200, mime, "");  // 只发响应头

  // 自己实现可靠写入：检查 write 返回值，短写时重试
  // sendContent_P 内部 sendSize 短写后丢弃数据，导致末尾截断
  const size_t CHUNK = 1000;
  char buf[CHUNK];
  for (size_t off = 0; off < len; off += CHUNK) {
    size_t n = (len - off < CHUNK) ? (len - off) : CHUNK;
    memcpy_P(buf, data + off, n);  // PROGMEM → RAM
    size_t written = 0;
    while (written < n) {
      int w = client.write(buf + written, n - written);
      if (w > 0) {
        written += w;
      } else {
        delay(1);  // 发送缓冲区满，等 lwip 腾空
      }
      yield();
    }
  }
}

void handleRoot() {
  sendChunked_P("text/html", WebAssets::index_html, WebAssets::index_html_len);
}

void handleCss() {
  sendChunked_P("text/css", WebAssets::style_css, WebAssets::style_css_len);
}

void handleJs() {
  sendChunked_P("application/javascript", WebAssets::app_js, WebAssets::app_js_len);
}

// 帮助文档（markdown 纯文本，AI Agent 和 WebUI 帮助页共用同一份内容）
// 返回 text/plain + UTF-8，避免浏览器误渲染 markdown
void handleHelp() {
  // help.md 约 17KB，跨 tailscale (PMTU 1280) 时 send_P 一次性发送会 IP 分片丢包
  // 复用 sendChunked_P 的分块可靠写入逻辑
  sendChunked_P("text/plain; charset=utf-8", WebAssets::help_md, WebAssets::help_md_len);
}

// 重启原因枚举转字符串（ESP32 使用 esp_reset_reason() API）
String getResetReasonStr() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:    return "Power on";
    case ESP_RST_EXT:        return "External reset";
    case ESP_RST_SW:         return "Software restart";
    case ESP_RST_PANIC:      return "Panic";
    case ESP_RST_INT_WDT:    return "Interrupt watchdog";
    case ESP_RST_TASK_WDT:   return "Task watchdog";
    case ESP_RST_WDT:        return "Watchdog";
    case ESP_RST_DEEPSLEEP:  return "Deep sleep";
    case ESP_RST_BROWNOUT:   return "Brownout";
    case ESP_RST_SDIO:       return "SDIO";
    case ESP_RST_USB:        return "USB";
    case ESP_RST_JTAG:       return "JTAG";
    case ESP_RST_EFUSE:      return "EFuse error";
    case ESP_RST_PWR_GLITCH: return "Power glitch";
    case ESP_RST_CPU_LOCKUP: return "CPU lockup";
    default:                 return "Unknown";
  }
}

void handleStatus() {
  String json = "{";
  const char* stateStr = "idle";
  if (g_state == STATE_BURNING) stateStr = "burning";
  else if (g_state == STATE_MONITORING) stateStr = "monitoring";
  else if (g_state == STATE_SNIFFING) stateStr = "sniffing";
  json += "\"state\":\"";
  json += stateStr;
  json += "\",\"config_mode\":" + String(g_in_config_mode ? "true" : "false");
  json += ",\"burn\":{";
  json += "\"percent\":" + String(g_burn.percent);
  json += ",\"current_block\":" + String(g_burn.current_block);
  json += ",\"total_blocks\":" + String(g_burn.total_blocks);
  json += ",\"done\":" + String(g_burn.done ? "true" : "false");
  json += ",\"error\":\"" + jsonEscape(g_burn.error) + "\"";
  json += "},\"monitor\":{";
  json += "\"active\":" + String(g_state == STATE_MONITORING ? "true" : "false");
  json += ",\"baud\":" + String(g_monitor_baud);
  json += ",\"bytes_received\":" + String(g_monitor_bytes_total);
  json += "},\"sniffer\":{";
  json += "\"active\":" + String(g_state == STATE_SNIFFING ? "true" : "false");
  json += ",\"channel\":" + String(g_sniffer_channel);
  json += ",\"baud\":" + String(g_sniffer_baud);
  json += ",\"buffer_size\":" + String(SNIFFER_BUFFER_SIZE);
  json += ",\"bytes_received\":" + String(g_sniffer_total_rx);
  json += ",\"bytes_sent\":" + String(g_sniffer_total_sent);
  json += ",\"dropped_bytes\":" + String(g_sniffer_drop_bytes);
  json += ",\"drop_count\":" + String(g_sniffer_drop_count);
  json += "},\"wifi\":{";
  json += "\"ssid\":\"" + jsonEscape(WiFi.SSID()) + "\"";
  if (WiFi.status() == WL_CONNECTED) {
    json += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
    json += ",\"rssi\":" + String(WiFi.RSSI());
    json += ",\"mode\":\"sta\"";
  } else if (WiFi.getMode() == WIFI_AP) {
    json += ",\"ip\":\"" + WiFi.softAPIP().toString() + "\"";
    json += ",\"rssi\":0";
    json += ",\"mode\":\"ap\"";
  } else {
    json += ",\"ip\":\"0.0.0.0\",\"rssi\":0,\"mode\":\"none\"";
  }
  json += "},\"uptime\":" + String(millis() / 1000);
  json += ",\"task_id\":" + String(g_burn_task_id);
  json += ",\"burn_pending\":" + String(g_burn_pending ? "true" : "false");
  // 当前时间（epoch 秒，已授时为北京时间 CST-8；未授时返回 0）
  json += ",\"time\":" + String((uint32_t)time(nullptr));
  // 设备名称（浏览器标签显示）
  json += ",\"device_name\":\"" + jsonEscape(g_config.device_name) + "\"";
  // 固件版本号 + 编译日期（编译标识，自动生成）
  json += ",\"version\":\"" + String(FIRMWARE_VERSION) + "\"";
  json += ",\"build_time\":\"" + String(BUILD_TIME) + "\"";
  // Flash 资源占用
  uint32_t sketchSize = ESP.getSketchSize();
  uint32_t sketchFree = ESP.getFreeSketchSpace();
  json += ",\"flash\":{\"sketch_size\":" + String(sketchSize);
  json += ",\"sketch_free\":" + String(sketchFree);
  json += ",\"chip_size\":" + String(ESP.getFlashChipSize()) + "}";
  // 内存信息：free_heap + RAM 占用百分比（ESP32-SOLO-1 共 320KB DRAM）
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t ramSize = 327680;  // ESP32-SOLO-1 总 RAM 320KB
  uint32_t ramUsed = ramSize - freeHeap;
  float ramPct = (float)ramUsed / ramSize * 100;
  json += ",\"memory\":{\"free_heap\":" + String(freeHeap);
  json += ",\"ram_size\":" + String(ramSize);
  json += ",\"ram_used\":" + String(ramUsed);
  json += ",\"ram_pct\":" + String(ramPct, 1) + "}";
  // 硬件信息：芯片型号/版本/CPU频率/MAC/主机名/AP名称
  json += ",\"hardware\":{";
  json += "\"chip_model\":\"ESP32-S0WD\"";  // ESP32-SOLO-1 单核
  json += ",\"chip_revision\":" + String(ESP.getChipRevision());
  json += ",\"cpu_freq\":" + String(ESP.getCpuFreqMHz());
  json += ",\"flash_size\":" + String(ESP.getFlashChipSize());
  json += ",\"mac\":\"" + WiFi.macAddress() + "\"";
  json += ",\"hostname\":\"" + jsonEscape(WiFi.getHostname()) + "\"";
  json += ",\"ap_name\":\"" + jsonEscape(g_default_ap_name) + "\"";
  json += ",\"sdk_version\":\"" + jsonEscape(ESP.getSdkVersion()) + "\"";
  json += "}";
  // 重启原因（诊断烧录崩溃用）
  json += ",\"reset_reason\":\"" + jsonEscape(getResetReasonStr()) + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleGetConfig() {
  String json = "{";
  json += "\"wifi_ssid\":\"" + jsonEscape(g_config.wifi_ssid) + "\"";
  json += ",\"wifi_password\":\"" + jsonEscape(g_config.wifi_password) + "\"";
  json += ",\"device_name\":\"" + jsonEscape(g_config.device_name) + "\"";
  json += ",\"verify\":" + String(g_config.verify ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handlePostConfig() {
  String body = server.arg("plain");
  // 合并保存：只更新请求中出现的字段
  String ssid = g_config.wifi_ssid;
  String pwd = g_config.wifi_password;
  String deviceName = g_config.device_name;
  uint8_t verify = g_config.verify;
  // 通过查找 "key": 是否存在判断字段是否出现
  if (body.indexOf("\"wifi_ssid\"") >= 0)     ssid = jsonGetString(body, "wifi_ssid");
  if (body.indexOf("\"wifi_password\"") >= 0) pwd = jsonGetString(body, "wifi_password");
  if (body.indexOf("\"device_name\"") >= 0)   deviceName = jsonGetString(body, "device_name");
  if (body.indexOf("\"verify\"") >= 0)        verify = jsonGetBool(body, "verify", false) ? 1 : 0;
  saveConfig(ssid, pwd, deviceName, verify);
  g_config.wifi_ssid = ssid;
  g_config.wifi_password = pwd;
  g_config.device_name = deviceName;
  g_config.verify = verify;
  server.send(200, "application/json", "{\"success\":true}");
}

void handleUpload() {
  HTTPUpload& upload = server.upload();
  if (!g_littlefs_ok) {
    g_upload_error = true;
    return;  // LittleFS 不可用，上传拒绝
  }
  if (upload.status == UPLOAD_FILE_START) {
    String filename = upload.filename;
    if (filename.length() == 0) filename = "firmware.bin";
    // 去掉路径分隔符，只保留文件名
    int slash = filename.lastIndexOf('/');
    if (slash >= 0) filename = filename.substring(slash + 1);
    slash = filename.lastIndexOf('\\');
    if (slash >= 0) filename = filename.substring(slash + 1);
    g_upload_filename = filename;
    g_upload_error = false;
    g_upload_rejected_hex = false;
    // API 上传不支持 .hex：浏览器端会自动 hex2bin，但 API（curl/Agent）不会
    // 直接拒绝并返回提示，避免 AI 把 .hex 当 BIN 烧录导致 CC2530 异常
    String lowerName = filename;
    lowerName.toLowerCase();
    if (lowerName.endsWith(".hex")) {
      g_upload_rejected_hex = true;
      Serial.printf("Upload rejected (.hex not supported via API): %s\n", filename.c_str());
      return;  // 不打开文件
    }
    // 删除同名旧文件
    if (LittleFS.exists("/" + filename)) {
      LittleFS.remove("/" + filename);
    }
    g_upload_file = LittleFS.open("/" + filename, "w");
    if (!g_upload_file) {
      g_upload_error = true;
    }
    Serial.printf("Upload start: %s\n", filename.c_str());
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    // .hex 被拒绝时跳过写入（multipart 数据仍会到达，但不落盘）
    if (g_upload_rejected_hex) return;
    if (g_upload_file && !g_upload_error) {
      size_t written = g_upload_file.write(upload.buf, upload.currentSize);
      if (written != upload.currentSize) {
        g_upload_error = true;
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (g_upload_file) {
      g_upload_file.close();
    }
    // .hex 拒绝响应：返回 400 + hex2bin 提示（Agent 友好）
    if (g_upload_rejected_hex) {
      g_upload_rejected_hex = false;
      String resp = "{\"error\":\"hex_not_supported\","
                    "\"message\":\"API 不支持 .hex 直传，请先在客户端转换为 .bin 再上传\","
                    "\"hint\":\"浏览器端上传 .hex 会自动 hex2bin；API 调用需自行转换。"
                    "算法：1) 按 Intel HEX 解析每行(冒号开头)，取 count/addr/type/data/checksum；"
                    "2) type=0x00 数据记录写入 baseAddr+bAddr；type=0x04 设置 baseAddr=data<<16；"
                    "type=0x02 设置 baseAddr=data<<4；type=0x01 结束；校验和=(sum(除最后字节))&0xFF 应为0；"
                    "3) 收集所有数据按地址排序，缺失地址填 0xFF；"
                    "4) 尾部填充 0xFF 到 256KB(0x40000) 以适配 CC2530F256。"
                    "可参考 data/app.js 中的 hex2bin() / parseHexToMap() / mapToBin() 实现。\"}";
      server.send(400, "application/json", resp);
      return;
    }
    Serial.printf("Upload end: %u bytes, error=%d\n", upload.totalSize, g_upload_error);
    // ============================================================
    // 可靠发送上传响应：绕过 server.send()，手动写入 HTTP 响应
    // ============================================================
    // 根因：ESP32 WebServer 的 _currentClient.write() 在 WiFi 发送缓冲区满时
    //       返回 0，数据丢失且无重试。server.send() 不检查 write() 返回值，
    //       导致响应丢失，浏览器报 "Failed to fetch"。
    //       server.client() 返回 WiFiClient 副本，setTimeout 设置的是副本的
    //       成员变量，不影响 _currentClient。因此必须手动发送并重试。
    int code = g_upload_error ? 500 : 200;
    String body;
    if (g_upload_error) {
      body = "{\"error\":\"write failed\"}";
    } else {
      body = "{\"success\":true,\"filename\":\"" + jsonEscape(g_upload_filename) +
             "\",\"size\":" + String(upload.totalSize) + "}";
    }
    String header = "HTTP/1.1 " + String(code) + (code == 200 ? " OK" : " Error") + "\r\n";
    header += "Content-Type: application/json\r\n";
    header += "Content-Length: " + String(body.length()) + "\r\n";
    header += "Connection: close\r\n";
    header += "\r\n";
    String full = header + body;
    // 获取 client 并设置超时（副本的 socket 与 _currentClient 共享）
    WiFiClient client = server.client();
    client.setTimeout(10000);
    const char* data = full.c_str();
    size_t len = full.length();
    size_t written = 0;
    unsigned long startMs = millis();
    while (written < len && millis() - startMs < 10000 && client.connected()) {
      int w = client.write(data + written, len - written);
      if (w > 0) {
        written += w;
      } else {
        delay(2);
      }
      yield();
    }
    Serial.printf("Upload response: %u/%u bytes sent (code=%d, %s)\n",
                  (unsigned)written, (unsigned)len, code,
                  written == len ? "OK" : "INCOMPLETE");
  }
}

void handleBurn() {
  if (g_state != STATE_IDLE || g_burn_pending) {
    server.send(409, "application/json", "{\"error\":\"busy\"}");
    return;
  }
  String body = server.arg("plain");
  // 强制校验：API 烧录必须 verify=true，忽略客户端传的 false
  // 避免 AI 跳过校验导致 CC2530 烧录异常未被发现
  bool verify = true;
  String filename = jsonGetString(body, "filename");
  if (filename.length() == 0) filename = "firmware.bin";

  if (!LittleFS.exists("/" + filename)) {
    server.send(404, "application/json", "{\"error\":\"file not found\"}");
    return;
  }

  File f = LittleFS.open("/" + filename, "r");
  uint32_t totalBlocks = (f.size() + 511) / 512;
  f.close();

  // 强制异步：立即返回 task_id，烧录在 loop() 中执行
  // 原同步模式会阻塞 HTTP ~90 秒（256KB BIN），期间 /api/status 等接口全部超时
  // AI Agent 调用时极易触发假死，故移除同步分支。?async=1 参数仍兼容但不再必需
  g_burn_task_id++;
  g_burn_pending = true;
  g_burn_pending_filename = filename;
  g_burn_pending_verify = verify;
  String resp = "{\"success\":true,\"async\":true,\"task_id\":" + String(g_burn_task_id) +
                ",\"total_blocks\":" + String(totalBlocks) +
                ",\"verify\":true,\"verify_forced\":true}";
  server.send(202, "application/json", resp);
  Serial.printf("Burn queued: task=%u file=%s verify=%d(forced) blocks=%u\n",
                g_burn_task_id, filename.c_str(), verify, totalBlocks);
}

void handleNvReset() {
  if (g_state != STATE_IDLE || g_burn_pending || g_nvreset_pending) {
    server.send(409, "application/json", "{\"error\":\"busy\"}");
    return;
  }
  g_nvreset_pending = true;
  server.send(202, "application/json", "{\"success\":true,\"async\":true,\"info\":\"清除配网：读取 Flash → 清除 NV → 写回\"}");
  Serial.println("NV reset queued");
}

void handleBackup() {
  if (g_state != STATE_IDLE || g_burn_pending || g_nvreset_pending || g_backup_pending) {
    server.send(409, "application/json", "{\"error\":\"busy\"}");
    return;
  }
  g_backup_pending = true;
  server.send(202, "application/json", "{\"success\":true,\"async\":true,\"info\":\"备份固件：读取 Flash 保存到 LittleFS\"}");
  Serial.println("Backup queued");
}

void handleMonitor() {
  if (g_state != STATE_IDLE) {
    server.send(409, "application/json", "{\"error\":\"busy\"}");
    return;
  }
  String body = server.arg("plain");
  uint32_t baud = (uint32_t)jsonGetInt(body, "baud", 115200);
  bool autoReset = jsonGetBool(body, "auto_reset", false);  // 默认非侵入式，不自动复位目标设备
  if (baud < 9600 || baud > 230400) {
    server.send(400, "application/json", "{\"error\":\"invalid baud\"}");
    return;
  }
  server.send(200, "application/json", "{\"success\":true,\"baud\":" + String(baud) + "}");
  digitalWrite(LED, HIGH);
  enterMonitorMode(baud, autoReset);
}

void handleStop() {
  if (g_state == STATE_MONITORING) {
    exitMonitorMode();
    digitalWrite(LED, LOW);
  } else if (g_state == STATE_SNIFFING) {
    exitSnifferMode();
  }
  server.send(200, "application/json", "{\"success\":true}");
}

// 手动复位 CC2530（通过 GPIO5/RESETn）
// 监控中也可调用：复位后 CC2530 重新启动，可捕获 main() 日志
void handleResetCC2530() {
  if (g_state == STATE_BURNING) {
    server.send(409, "application/json", "{\"error\":\"busy\"}");
    return;
  }
  // 监控中复位：先暂停接收，复位后继续
  // 保留 g_monitor_ring_total / g_monitor_bytes_total 累计计数：
  // Agent 用 /api/monitor/buffer?since=N 断点续传时，offset 语义保持单调递增，
  // 手动复位只清前端显示（通过 monitor_reset 事件），不影响后端计数
  bool wasMonitoring = (g_state == STATE_MONITORING);
  if (wasMonitoring) {
    // 推送残留数据
    if (g_monitor_len > 0) pushMonitorData();
    g_monitor_len = 0;
  }
  resetCC2530();
  server.send(200, "application/json", "{\"success\":true}");
  // 如果在监控中，通知前端清空日志区
  if (wasMonitoring) {
    String json = "{\"type\":\"monitor_reset\"}";
    sseSend(json);
  }
}

// Agent 友好：获取监控日志环形缓冲（支持断点续传）
// GET /api/monitor/buffer                      - 返回最近 max_bytes 字节（默认 4096）
// GET /api/monitor/buffer?since=N              - 返回从累计字节 N 之后的数据
// GET /api/monitor/buffer?since=N&max_bytes=M  - 限制单次返回字节数
// 响应：{"success":true,"total":N,"offset":N,"bytes":N,"truncated":false,"data":"<base64>"}
// 流式分块输出，避免 8KB base64 (~11KB) + String 拼接导致堆碎片化/OOM
void handleMonitorBuffer() {
  uint32_t since = 0;
  if (server.hasArg("since")) {
    since = (uint32_t)strtoul(server.arg("since").c_str(), NULL, 10);
  }
  // 限制单次返回最大字节数。原版可能返回全部 8KB → base64 11KB + String 拼接 22KB
  // 临时堆分配过大易碎片化，默认 4KB，可下调
  uint32_t maxBytes = 4096;
  if (server.hasArg("max_bytes")) {
    long mb = strtol(server.arg("max_bytes").c_str(), NULL, 10);
    if (mb > 0 && mb <= 8192) maxBytes = (uint32_t)mb;
  }

  uint32_t total = g_monitor_ring_total;
  uint32_t buffered = (total < MONITOR_RING_SIZE) ? total : MONITOR_RING_SIZE;
  uint32_t oldest = total - buffered;  // 缓冲中最早字节对应的累计偏移

  uint32_t startOffset, bytesToRead;
  bool truncated = false;
  uint32_t missed = 0;

  if (since >= total) {
    // 没有新数据，直接发完整 JSON（小）
    String json = "{\"success\":true,\"total\":" + String(total) +
                  ",\"offset\":" + String(total) +
                  ",\"bytes\":0,\"truncated\":false,\"data\":\"\"}";
    server.send(200, "application/json", json);
    return;
  } else if (since < oldest) {
    // 请求的偏移已超出缓冲范围（数据被覆盖）
    startOffset = oldest;
    bytesToRead = buffered;
    truncated = true;
    missed = oldest - since;
  } else {
    startOffset = since;
    bytesToRead = total - since;
  }

  // 限制单次返回
  if (bytesToRead > maxBytes) bytesToRead = maxBytes;

  // 流式分块输出（chunked transfer）
  // 先写头部字段，再分块写 base64 数据，最后闭合 JSON
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  String header = "{\"success\":true,\"total\":" + String(total) +
                  ",\"offset\":" + String(startOffset) +
                  ",\"bytes\":" + String(bytesToRead) +
                  ",\"truncated\":" + String(truncated ? "true" : "false");
  if (missed > 0) header += ",\"missed\":" + String(missed);
  header += ",\"data\":\"";
  server.sendContent(header);

  // 分块输出 base64：768 字节原始 -> 1024 base64 字符，单次堆分配 < 1.5KB
  uint32_t startInRing = startOffset % MONITOR_RING_SIZE;
  uint32_t remaining = bytesToRead;
  const uint16_t CHUNK_RAW = 768;
  uint8_t chunk[CHUNK_RAW];
  while (remaining > 0) {
    uint32_t thisChunk = (remaining > CHUNK_RAW) ? CHUNK_RAW : remaining;
    for (uint32_t i = 0; i < thisChunk; i++) {
      chunk[i] = g_monitor_ring[(startInRing + i) % MONITOR_RING_SIZE];
    }
    String b64 = base64Encode(chunk, thisChunk);
    server.sendContent(b64);
    startInRing = (startInRing + thisChunk) % MONITOR_RING_SIZE;
    remaining -= thisChunk;
    yield();  // 喂狗 + 让 WiFi 任务运行
  }
  server.sendContent("\"}");
}

void handleFiles() {
  if (!g_littlefs_ok) {
    server.send(200, "application/json", "{\"success\":true,\"files\":[]}");
    return;
  }
  String json = "{\"success\":true,\"files\":[";
  bool first = true;
  File root = LittleFS.open("/");
  File entry = root.openNextFile();
  while (entry) {
    String name = entry.name();
    // ESP32 LittleFS 的 name() 可能返回带 "/" 前缀，统一去掉
    if (name.startsWith("/")) name = name.substring(1);
    // 跳过配置文件和 WebUI 静态文件
    if (name == "config.json" || name == "index.html" ||
        name == "style.css" || name == "app.js") {
      entry = root.openNextFile();
      continue;
    }
    if (!first) json += ",";
    first = false;
    json += "{\"name\":\"" + jsonEscape(name) + "\"";
    json += ",\"size\":" + String(entry.size());
    // ESP32 LittleFS 不支持文件创建时间，用当前系统时间代替
    // （已授时为北京时间 CST-8；未授时返回 0，前端 fallback 显示 "刚刚"）
    json += ",\"time\":" + String((uint32_t)time(nullptr));
    json += "}";
    entry = root.openNextFile();
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleDeleteFile() {
  String name = server.pathArg(0);
  if (name.length() == 0) {
    server.send(400, "application/json", "{\"error\":\"no filename\"}");
    return;
  }
  name = urlDecode(name);
  // 安全检查：禁止删除 WebUI 文件和配置，禁止路径穿越
  if (name == "config.json" || name == "index.html" ||
      name == "style.css" || name == "app.js" || name.indexOf('/') >= 0 ||
      name.indexOf('\\') >= 0) {
    server.send(403, "application/json", "{\"error\":\"forbidden\"}");
    return;
  }
  if (LittleFS.remove("/" + name)) {
    server.send(200, "application/json", "{\"success\":true}");
  } else {
    server.send(404, "application/json", "{\"error\":\"not found\"}");
  }
}

// GET /api/files/{name}  下载已上传的固件文件
//   返回文件内容（application/octet-stream），Content-Disposition: attachment
//   复用 sendChunked_P 的可靠写入方式，避免 chunkedResponseModeStart + sendContent_P
//   配合在跨 tailscale 时丢包
void handleGetFile() {
  String name = server.pathArg(0);
  if (name.length() == 0) {
    server.send(400, "application/json", "{\"error\":\"no filename\"}");
    return;
  }
  name = urlDecode(name);
  // 安全检查：禁止下载 WebUI 文件和配置，禁止路径穿越
  if (name == "config.json" || name == "index.html" ||
      name == "style.css" || name == "app.js" || name.indexOf('/') >= 0 ||
      name.indexOf('\\') >= 0) {
    server.send(403, "application/json", "{\"error\":\"forbidden\"}");
    return;
  }
  String path = "/" + name;
  if (!LittleFS.exists(path)) {
    server.send(404, "application/json", "{\"error\":\"not found\"}");
    return;
  }
  File f = LittleFS.open(path, "r");
  if (!f) {
    server.send(500, "application/json", "{\"error\":\"open failed\"}");
    return;
  }
  size_t fileSize = f.size();
  WiFiClient client = server.client();
  client.setNoDelay(true);
  client.setTimeout(5000);
  server.setContentLength(fileSize);
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
  server.sendHeader("Connection", "close");
  server.send(200, "application/octet-stream", "");  // 只发响应头

  // 分块可靠写入（与 sendChunked_P 同款逻辑）
  const size_t CHUNK = 1000;
  uint8_t buf[CHUNK];
  while (fileSize > 0) {
    size_t n = (fileSize < CHUNK) ? fileSize : CHUNK;
    int read = f.read(buf, n);
    if (read <= 0) break;
    size_t written = 0;
    while (written < (size_t)read) {
      int w = client.write(buf + written, read - written);
      if (w > 0) {
        written += w;
      } else {
        delay(1);
      }
      yield();
    }
    fileSize -= read;
  }
  f.close();
}

void handleReboot() {
  server.send(200, "application/json", "{\"success\":true}");
  delay(200);
  ESP.restart();
}

// WiFi 扫描：返回周围可用网络列表
void handleWifiScan() {
  Serial.println("WiFi scan start...");
  int n = WiFi.scanNetworks();
  Serial.printf("WiFi scan done, found %d networks\n", n);
  String json = "{\"success\":true,\"networks\":[";
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";
    String ssid = WiFi.SSID(i);
    int rssi = WiFi.RSSI(i);
    int enc = (int)WiFi.encryptionType(i);
    // 用 32 字节限制（WiFi SSID 最大长度）
    json += "{\"ssid\":\"" + jsonEscape(ssid) + "\"";
    json += ",\"rssi\":" + String(rssi);
    json += ",\"encrypted\":" + String(enc != WIFI_AUTH_OPEN ? "true" : "false");
    json += ",\"enc_type\":" + String(enc);
    json += "}";
  }
  json += "]}";
  server.send(200, "application/json", json);
  // 释放扫描结果内存
  WiFi.scanDelete();
}

// WiFi 连接：尝试连接指定 SSID/密码，成功后保存配置并切换到 STA 模式
void handleWifiConnect() {
  String body = server.arg("plain");
  String ssid = jsonGetString(body, "ssid");
  String pwd = jsonGetString(body, "password");
  if (ssid.length() == 0) {
    server.send(400, "application/json", "{\"error\":\"ssid required\"}");
    return;
  }
  if (ssid.length() > 32) {
    server.send(400, "application/json", "{\"error\":\"ssid too long\"}");
    return;
  }
  // 立即响应（连接过程在下面同步执行，前端通过 IP 切换感知）
  server.send(200, "application/json", "{\"success\":true,\"message\":\"connecting\"}");

  // 切换到 STA 模式并连接
  bool ok = switchToStaMode(ssid, pwd);
  if (ok) {
    // 推送 SSE 通知（如果还有客户端连着 AP 的话，会断开）
    String json = "{\"type\":\"wifi_connected\",\"ssid\":\"" + jsonEscape(ssid) +
                  "\",\"ip\":\"" + WiFi.localIP().toString() + "\"}";
    sseSend(json);
  } else {
    String json = "{\"type\":\"wifi_connect_failed\",\"ssid\":\"" + jsonEscape(ssid) + "\"}";
    sseSend(json);
  }
}

// ===== Sniffer API Handlers =====
// POST /api/sniffer/start  启动 sniffer 模式（必须 IDLE）
//   body: {"channel":11,"baud":115200}
//   返回: {"success":true,"channel":11,"baud":115200,"buffer_size":32768}
void handleSnifferStart() {
  if (g_state != STATE_IDLE) {
    server.send(409, "application/json", "{\"error\":\"busy\"}");
    return;
  }
  String body = server.arg("plain");
  long channel = jsonGetInt(body, "channel", 11);
  long baud = jsonGetInt(body, "baud", 115200);
  if (channel < 11 || channel > 26) {
    server.send(400, "application/json", "{\"error\":\"invalid channel\"}");
    return;
  }
  if (baud < 9600 || baud > 230400) {
    server.send(400, "application/json", "{\"error\":\"invalid baud\"}");
    return;
  }
  if (!enterSnifferMode((uint8_t)channel, (uint32_t)baud)) {
    server.send(503, "application/json", "{\"error\":\"out of memory\"}");
    return;
  }
  String json = "{\"success\":true,\"channel\":" + String((int)channel) +
                ",\"baud\":" + String((int)baud) +
                ",\"buffer_size\":" + String(SNIFFER_BUFFER_SIZE) + "}";
  server.send(200, "application/json", json);
}

// POST /api/sniffer/channel  切换抓包通道（必须 SNIFFING，且无 stream 客户端连接）
//   body: {"channel":15}
//   返回: {"success":true,"channel":15}
// 原理：ZBOSS sniffer 协议下行仅 1 字节通道号（11-26），sniffer 固件 serial_rx_inter_handler
//   收到后执行 zb_clear_sniffer + zb_start_sniffer 切换通道。
// 注意：stream 客户端连接期间 HTTP 单线程阻塞，本接口无法响应。PC 脚本应先断开 stream
//   再调本接口切换通道，然后重新连接 stream。
void handleSnifferChannel() {
  if (g_state != STATE_SNIFFING) {
    server.send(409, "application/json", "{\"error\":\"not sniffing\"}");
    return;
  }
  String body = server.arg("plain");
  long channel = jsonGetInt(body, "channel", -1);
  if (channel < 11 || channel > 26) {
    server.send(400, "application/json", "{\"error\":\"invalid channel\"}");
    return;
  }
  // 发送 1 字节通道号给 sniffer 固件（ZBOSS 协议下行，经 Serial2 TX → CC2530 P0_2）
  // ESP32 双串口架构：Serial2.write 不影响 Serial2 RX，可安全发送
  Serial2.write((uint8_t)channel);
  Serial2.flush();
  // 等待 sniffer 切换通道并清空 RF FIFO（zb_clear_sniffer + zb_start_sniffer）
  delay(200);
  // 清空 RX 缓冲（切换期间可能输出了状态字节，避免污染新通道的 pcap）
  while (Serial2.available()) Serial2.read();
  // 清空环形缓冲（避免残留的旧通道数据污染新通道的 pcap）
  g_sniffer_head = 0;
  g_sniffer_tail = 0;
  g_sniffer_drop_accum = 0;
  g_sniffer_channel = (uint8_t)channel;
  String json = "{\"success\":true,\"channel\":" + String((int)channel) + "}";
  server.send(200, "application/json", json);
}

// GET /api/sniffer/status  sniffer 状态 + 丢包统计
//   返回: {"active":true,"channel":11,"baud":115200,
//          "buffer_used":4096,"buffer_size":32768,
//          "bytes_received":102400,"bytes_sent":98304,
//          "dropped_bytes":4096,"drop_count":4,"uptime":60}
void handleSnifferStatus() {
  uint32_t used = (g_sniffer_head >= g_sniffer_tail)
      ? (g_sniffer_head - g_sniffer_tail)
      : (SNIFFER_BUFFER_SIZE - g_sniffer_tail + g_sniffer_head);
  uint32_t uptime = (g_state == STATE_SNIFFING)
      ? (uint32_t)((millis() - g_sniffer_start_ms) / 1000) : 0;
  String json = "{";
  json += "\"active\":" + String(g_state == STATE_SNIFFING ? "true" : "false");
  json += ",\"channel\":" + String(g_sniffer_channel);
  json += ",\"baud\":" + String(g_sniffer_baud);
  json += ",\"buffer_used\":" + String(used);
  json += ",\"buffer_size\":" + String(SNIFFER_BUFFER_SIZE);
  json += ",\"bytes_received\":" + String(g_sniffer_total_rx);
  json += ",\"bytes_sent\":" + String(g_sniffer_total_sent);
  json += ",\"dropped_bytes\":" + String(g_sniffer_drop_bytes);
  json += ",\"drop_count\":" + String(g_sniffer_drop_count);
  json += ",\"uptime\":" + String(uptime);
  json += "}";
  server.send(200, "application/json", json);
}

// GET /api/sniffer/stream  HTTP chunked 流式传输 sniffer 原始字节
//   持续推送，客户端断开后保持 sniffer 运行（允许多次连接）
//   仅允许 1 个客户端同时连接，避免多客户端分流数据
void handleSnifferStream() {
  if (g_state != STATE_SNIFFING) {
    server.send(409, "application/json", "{\"error\":\"not sniffing\"}");
    return;
  }
  if (g_sniffer_client_active) {
    server.send(409, "application/json", "{\"error\":\"stream busy\"}");
    return;
  }
  WiFiClient client = server.client();
  g_sniffer_client = client;
  g_sniffer_client_active = true;
  // 发送 chunked 响应头
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/octet-stream");
  client.println("Transfer-Encoding: chunked");
  client.println("Cache-Control: no-cache");
  client.println("Connection: keep-alive");
  client.println();

  uint8_t buf[1024];
  // 循环推送：先读 Serial2 入缓冲 → 读缓冲 → chunked 写客户端
  // 注意：此 handler 阻塞期间 loop() 不执行，必须在循环内自行读 Serial2，
  // 否则数据丢失。缓冲在此充当 Serial2 与 WiFi 之间的解耦队列。
  // 使用逐字节 Serial2.read()（与 handleMonitoring 一致，避免 readBytes 的潜在问题）
  // 循环中调用 sseLoop() 推送 SSE 事件，避免状态更新滞后
  while (client.connected() && g_state == STATE_SNIFFING) {
    // 1. 从 Serial2 读取数据入缓冲（逐字节读取，与 handleSniffing 一致）
    handleSniffing();
    // 2. 从缓冲读取并 chunked 发送
    size_t n = snifferRead(buf, sizeof(buf));
    if (n > 0) {
      // chunked 格式: <hex长度>\r\n<data>\r\n
      client.printf("%X\r\n", (unsigned int)n);
      size_t written = 0;
      while (written < n) {
        int w = client.write(buf + written, n - written);
        if (w > 0) {
          written += w;
        } else {
          delay(1);
        }
        yield();
      }
      client.print("\r\n");
      g_sniffer_total_sent += n;
    } else {
      // 无数据，短暂等待避免 busy-loop
      delay(5);
    }
    // 推送 SSE 事件（如 sniffer_start/sniffer_stop），避免状态更新滞后
    // handleSnifferStream 阻塞期间 loop() 不执行，sseLoop() 不被调用，
    // 导致 SSE 事件堆积、前端状态显示延迟
    sseLoop();
    yield();  // 喂狗 + WiFi 任务
  }
  // 结束 chunk
  if (client.connected()) {
    client.println("0\r\n\r\n");
  }
  g_sniffer_client_active = false;
  // ESP32 双串口架构：Serial（调试日志）与 Serial2（CC2530 数据）物理分离，
  // 此处可安全使用 Serial.println 调试输出，不影响 sniffer RX。
  // stream 客户端断开后 sniffer 继续运行，loop() 中
  // handleSniffing() 持续读取 Serial2 数据入缓冲，等待下一个 stream 客户端连接。
}

void initHttpRoutes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/style.css", HTTP_GET, handleCss);
  server.on("/app.js", HTTP_GET, handleJs);
  server.on("/api/help", HTTP_GET, handleHelp);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/config", HTTP_GET, handleGetConfig);
  server.on("/api/config", HTTP_POST, handlePostConfig);
  server.on("/api/upload", HTTP_POST, [](){ /* 响应在 handleUpload END 阶段发 */ }, handleUpload);
  server.on("/api/burn", HTTP_POST, handleBurn);
  server.on("/api/nvreset", HTTP_POST, handleNvReset);
  server.on("/api/backup", HTTP_POST, handleBackup);
  server.on("/api/monitor", HTTP_POST, handleMonitor);
  server.on("/api/stop", HTTP_POST, handleStop);
  server.on("/api/reset", HTTP_POST, handleResetCC2530);
  server.on("/api/monitor/buffer", HTTP_GET, handleMonitorBuffer);
  // Sniffer 抓包接口（详见 CCLoader_Sniffer抓包改造需求.md）
  server.on("/api/sniffer/start", HTTP_POST, handleSnifferStart);
  server.on("/api/sniffer/channel", HTTP_POST, handleSnifferChannel);
  server.on("/api/sniffer/stream", HTTP_GET, handleSnifferStream);
  server.on("/api/sniffer/status", HTTP_GET, handleSnifferStatus);
  server.on("/api/files", HTTP_GET, handleFiles);
  // /api/files/{name} - 用 UriBraces 通配符（UriRegex 在 ESP32 std::regex 下会崩溃）
  server.on(UriBraces("/api/files/{}"), HTTP_GET, handleGetFile);
  server.on(UriBraces("/api/files/{}"), HTTP_DELETE, handleDeleteFile);
  server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
  server.on("/api/wifi/connect", HTTP_POST, handleWifiConnect);
  server.on("/api/reboot", HTTP_POST, handleReboot);
  server.onNotFound([](){
    // 配网模式下：所有未识别 URL 返回主页（captive portal）
    // 手机/电脑连 AP 后访问任意 URL 会自动弹出配网页
    if (g_in_config_mode) {
      // 对 HTML 请求返回主页，对其他请求返回 302 重定向
      String accept = server.header("Accept");
      if (accept.indexOf("text/html") >= 0 || accept.indexOf("text/css") >= 0 ||
          accept.length() == 0) {
        handleRoot();
      } else {
        // API 请求或资源请求在配网模式下返回 404
        server.send(404, "application/json", "{\"error\":\"not found, in config mode\"}");
      }
    } else {
      server.send(404, "application/json", "{\"error\":\"not found\"}");
    }
  });
  // 收集客户端 header 用于 captive portal 判断
  const char* headerKeys[] = {"Accept", "User-Agent"};
  server.collectHeaders(headerKeys, sizeof(headerKeys) / sizeof(headerKeys[0]));
}

// ===== setup / loop =====
void setup() {
  ProgrammerInit();
  // Serial 重映射到板载 USB（ESP32-SOLO-1 开发板 CH340C 接 IO22/IO23，非默认 IO1/IO3）
  // 参数：baud, config, RX=IO23, TX=IO22
  Serial.begin(115200, SERIAL_8N1, 23, 22);
  delay(100);
  Serial.println("\nCCLoader WebUI booting...");
  // Serial2 初始化（CC2530 监控/sniffer 数据接收，IO16 RX）
  // ESP32 UART 架构关键点：
  //   硬件 FIFO (128字节) → IDF事件任务 → 软件RingBuffer → available()/read()
  //   rxfifo_full_thrhd 控制何时触发 FIFO→RingBuffer 的数据搬移
  //   默认 120 字节才触发，CC2530 小数据包(<120B)会卡在 FIFO 中无法被 available() 检测到
  //   设为 1 实现逐字节触发，与 ESP8266 的 Serial 行为一致
  // setRxBufferSize 必须在 begin() 之前调用
  // Serial2.begin() 使用 IO16/IO17（UART2 IO MUX 默认引脚），无需 GPIO Matrix 重映射
  Serial2.setRxBufferSize(4096);
  Serial2.begin(115200, SERIAL_8N1, CC_SERIAL_RX, CC_SERIAL_TX, false, 20000UL, 1);
  // 设置最短 RX 超时（1 symbol ≈ 0.01ms @ 115200），确保数据立即被搬到 RingBuffer
  Serial2.setRxTimeout(1);
  Serial.println("Serial2 initialized: RX=IO16, rxfifo_full=1, rxTimeout=1, rxBuf=4096");

  // 挂载 LittleFS
  // ESP32 首次烧录后 LittleFS 分区可能未格式化，begin() 失败时尝试 format
  if (LittleFS.begin()) {
    g_littlefs_ok = true;
    Serial.println("LittleFS mounted OK");
  } else {
    Serial.println("LittleFS mount failed, trying format...");
    if (LittleFS.format() && LittleFS.begin()) {
      g_littlefs_ok = true;
      Serial.println("LittleFS formatted and mounted OK");
    } else {
      g_littlefs_ok = false;
      Serial.println("LittleFS format failed! Running without filesystem (config/save/upload disabled)");
    }
  }

  loadConfig();

  initWiFi();

  // NTP 网络授时（北京时间 UTC+8）
  // LittleFS 创建文件时自动用 time(NULL) 作为时间戳，授时后 /api/files 的 time 字段不再为 0(1970)
  // configTime 非阻塞，NTP 在后台同步（STA 模式下生效；AP 模式无网则保持未授时）
  if (WiFi.status() == WL_CONNECTED) {
    configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org", "time.windows.com");
    Serial.println("NTP configured (CST-8), syncing in background...");
  }

  initHttpRoutes();
  // OTA 升级：访问 http://<ip>/update 上传 .bin 即可远程升级固件
  // LittleFS 保留，配置不丢失。升级后自动重启
  httpUpdater.setup(&server, "/update");
  sseServer.begin();
  server.begin();

  Serial.println("CCLoader WebUI ready");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
  }
  Serial.println("HTTP: 80, SSE: 81, OTA: /update");

  g_state = STATE_IDLE;
}

void loop() {
  // 备份固件：检测到 pending 标志后在 loop 中执行
  if (g_backup_pending && g_state == STATE_IDLE) {
    g_backup_pending = false;
    g_state = STATE_BURNING;
    g_burn.info = "";
    Serial.println("Starting backup...");
    backupCC2530();
    g_state = STATE_IDLE;
  }

  // 清除配网：检测到 pending 标志后在 loop 中执行
  if (g_nvreset_pending && g_state == STATE_IDLE) {
    g_nvreset_pending = false;
    g_state = STATE_BURNING;
    Serial.println("Starting NV reset...");
    nvResetCC2530();
    g_state = STATE_IDLE;
  }

  // 异步烧录：检测到 pending 标志后在 loop 中执行
  // burnFromLittleFS 内部会周期性调用 server.handleClient() 保持 HTTP 可响应
  if (g_burn_pending && g_state == STATE_IDLE) {
    g_burn_pending = false;
    g_state = STATE_BURNING;
    digitalWrite(LED, HIGH);
    Serial.printf("Starting async burn: %s\n", g_burn_pending_filename.c_str());
    burnFromLittleFS(g_burn_pending_filename, g_burn_pending_verify);
    digitalWrite(LED, LOW);
    g_state = STATE_IDLE;
    g_burn_pending_filename = "";
  }

  server.handleClient();
  sseLoop();

  switch (g_state) {
    case STATE_IDLE:
      // 空闲，WiFi+HTTP+SSE 在线，等待浏览器操作
      break;
    case STATE_BURNING:
      // 烧录在 HTTP handler 或 loop 异步分支中同步执行，这里不会进入
      break;
    case STATE_MONITORING:
      handleMonitoring();
      break;
    case STATE_SNIFFING:
      // 流式传输在 handleSnifferStream 内同步循环，这里仅在无客户端时读串口入缓冲
      handleSniffing();
      break;
  }
}
