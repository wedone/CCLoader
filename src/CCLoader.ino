/******************************************************************************
 * CCLoader WebUI - 基于 ESP8266 的 CC2530 烧录+监控一体机
 *
 * 接线（NodeMCU ESP8266，与 CCLib 兼容）：
 *   GPIO5  (D1) -> CC Pin 7 (RESETn)
 *   GPIO4  (D2) -> CC Pin 3 (DC)
 *   GPIO12 (D6) -> CC Pin 4 (DD)
 *   GPIO3  (RX) -> CC P0_3   (UART0 TX)  监控用
 *   GND         -> CC GND
 *   GPIO2  (D4) -> 板载蓝色 LED（状态指示）
 *
 * 工作模式：
 *   - IDLE：WiFi+HTTP+SSE 在线，等待浏览器操作
 *   - BURNING：从 LittleFS 读取 BIN 烧录到 CC2530，进度通过 SSE 推送
 *   - MONITORING：Serial 切换到 CC2530 波特率，接收 P0_3 日志并通过 SSE 推送
 *
 * 无外部库依赖：
 *   - HTTP/WebServer: ESP8266WebServer (端口 80)
 *   - 实时推送:       原生 WiFiServer SSE (端口 81)
 *   - JSON:           String 手工拼接/解析（仅顶层简单字段）
 *
 * 原有 CC Debug 协议函数（write_debug_byte ~ RunDUP）保持不变，仅数据源
 * 从 Serial 改为 LittleFS 文件。
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
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <uri/UriRegex.h>
#include <LittleFS.h>
#include <FS.h>
#include "web_assets.h"  // 内嵌 WebUI 静态资源（OTA 升级时一并更新）

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

// ===== 引脚配置（NodeMCU ESP8266，与 CCLib 兼容）=====
int DD = 12;     // GPIO12 = D6
int DC = 4;      // GPIO4  = D2
int RESET = 5;   // GPIO5  = D1
int LED = 2;     // GPIO2  = D4（板载蓝色 LED）

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
#pragma inline
void write_debug_byte(unsigned char data)
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

#pragma inline
unsigned char read_debug_byte(void)
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

#pragma inline
unsigned char wait_dup_ready(void)
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
  STATE_MONITORING
};
CCLoaderState g_state = STATE_IDLE;

// 全局对象：HTTP 服务器（80），SSE 服务器（81）
ESP8266WebServer server(80);
WiFiServer sseServer(81);
ESP8266HTTPUpdateServer httpUpdater;  // OTA 升级服务（/update）
#define SSE_MAX_CLIENTS 4
WiFiClient sseClients[SSE_MAX_CLIENTS];
bool sseActive[SSE_MAX_CLIENTS] = {false, false, false, false};

// 配置
struct Config {
  String wifi_ssid;
  String wifi_password;
  uint32_t monitor_baud;
  uint8_t verify;
};
Config g_config;

// 烧录状态
struct BurnState {
  uint32_t total_blocks;
  uint32_t current_block;
  uint8_t percent;
  String error;
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
fs::File g_upload_file;
bool g_upload_error = false;
bool g_upload_rejected_hex = false;  // API 上传 .hex 时拒绝（浏览器端会自动 hex2bin，API 不会）

// 配网模式标志：AP 模式下为 true，captive portal 启用
bool g_in_config_mode = false;

// 异步烧录：POST /api/burn?async=1 立即返回，烧录在 loop() 中执行
bool g_burn_pending = false;
String g_burn_pending_filename;
bool g_burn_pending_verify = false;
uint32_t g_burn_task_id = 0;  // 单调递增的 task_id

// 监控日志环形缓冲：支持 Agent 轮询 GET /api/monitor/buffer?since=N
// 缓存最近 8KB 日志，Agent 可断点续传获取
#define MONITOR_RING_SIZE 8192
uint8_t g_monitor_ring[MONITOR_RING_SIZE];
uint32_t g_monitor_ring_head = 0;   // 下一个写入位置 (mod SIZE)
uint32_t g_monitor_ring_total = 0;  // 累计写入字节数（单调递增，不取模）

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
  g_config.monitor_baud = 115200;
  g_config.verify = 0;

  if (LittleFS.exists("/config.json")) {
    fs::File f = LittleFS.open("/config.json", "r");
    if (f) {
      String s;
      while (f.available() && s.length() < 1024) s += (char)f.read();
      f.close();
      g_config.wifi_ssid = jsonGetString(s, "wifi_ssid");
      g_config.wifi_password = jsonGetString(s, "wifi_password");
      g_config.monitor_baud = (uint32_t)jsonGetInt(s, "monitor_baud", 115200);
      g_config.verify = jsonGetBool(s, "verify", false) ? 1 : 0;
    }
  }
}

void saveConfig(const String& ssid, const String& pwd, uint32_t baud, uint8_t verify) {
  String json = "{\n";
  json += "  \"wifi_ssid\": \"" + jsonEscape(ssid) + "\",\n";
  json += "  \"wifi_password\": \"" + jsonEscape(pwd) + "\",\n";
  json += "  \"monitor_baud\": " + String(baud) + ",\n";
  json += "  \"verify\": " + String(verify ? "true" : "false") + "\n";
  json += "}\n";
  fs::File f = LittleFS.open("/config.json", "w");
  if (f) {
    f.print(json);
    f.close();
  }
}

// ===== WiFi =====
// 进入配网模式：开放 AP + captive portal，用户连 CCLoader-Setup 后访问任意 URL 配网
void enterConfigMode(const char* reason) {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("CCLoader-Setup");  // 开放 AP，无密码
  g_in_config_mode = true;
  Serial.printf("Config mode (%s): AP 'CCLoader-Setup' open, IP: ", reason);
  Serial.println(WiFi.softAPIP());
}

void initWiFi() {
  if (g_config.wifi_ssid.length() == 0) {
    // 无配置，进入配网模式
    enterConfigMode("no config");
    return;
  }
  // 有配置，尝试连接 STA
  WiFi.mode(WIFI_STA);
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
  WiFi.mode(WIFI_STA);
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
    saveConfig(ssid, pwd, g_config.monitor_baud, g_config.verify);
    g_config.wifi_ssid = ssid;
    g_config.wifi_password = pwd;
    return true;
  }
  // 连接失败，回到 AP 配网模式
  Serial.println("Connect failed, back to config mode");
  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  WiFi.softAP("CCLoader-Setup");
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
void sseSend(const String& json) {
  for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
    if (sseActive[i] && sseClients[i].connected()) {
      sseClients[i].printf("event: message\ndata: %s\n\n", json.c_str());
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
  fs::File f = LittleFS.open("/" + filename, "r");
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
    // 原 32 块间隔在偶发慢块时可能踩 ESP8266 软件 WDT (~3.2s) 边界
    // 16 块 ≈ 0.8-1.6s 间隔，进度条每 3% 更新一次，仍流畅
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

// ===== CC2530 复位（通过 GPIO5/RESETn 控制，无需手动按按钮）=====
// 拉低 RESETn 10ms 再拉高，CC2530 重新从 main() 开始执行
void resetCC2530() {
  digitalWrite(DD, LOW);
  digitalWrite(DC, LOW);
  digitalWrite(RESET, LOW);
  delay(10);
  digitalWrite(RESET, HIGH);
  delay(10);
}

// ===== 监控模式：Serial 接收 CC2530 日志并推送 =====
// autoReset=true 时进入监控前自动复位 CC2530，可捕获 main() 启动日志
void enterMonitorMode(uint32_t baud, bool autoReset) {
  // 先复位 CC2530（在 Serial 切换前，确保从启动日志开始捕获）
  if (autoReset) {
    resetCC2530();
  }
  // 切换 Serial 到 CC2530 波特率，GPIO3 接收 P0_3 日志
  Serial.flush();
  Serial.end();
  delay(100);
  Serial.begin(baud);
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
}

void exitMonitorMode() {
  // 推送剩余数据
  if (g_monitor_len > 0) pushMonitorData();
  // 恢复 Serial 到默认波特率
  Serial.flush();
  Serial.end();
  delay(100);
  Serial.begin(115200);
  g_state = STATE_IDLE;
  g_monitor_len = 0;

  String json = "{\"type\":\"monitor_stop\"}";
  sseSend(json);
}

void handleMonitoring() {
  // 限制单次 loop 读取字节数，避免 CC2530 高速输出时 while(Serial.available())
  // 长时间占用 CPU，导致 server.handleClient() 被饿死、/api/status 等接口假死
  // 128 字节 ≈ 1ms @ 115200bps，足够让 loop() 每秒调度数百次 HTTP
  const uint16_t MAX_READ_PER_LOOP = 128;
  uint16_t readThisCall = 0;
  while (Serial.available() && readThisCall < MAX_READ_PER_LOOP) {
    uint8_t ch = Serial.read();
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
void handleRoot() {
  server.send_P(200, "text/html", WebAssets::index_html);
}

void handleCss() {
  server.send_P(200, "text/css", WebAssets::style_css);
}

void handleJs() {
  server.send_P(200, "application/javascript", WebAssets::app_js);
}

void handleStatus() {
  String json = "{";
  const char* stateStr = "idle";
  if (g_state == STATE_BURNING) stateStr = "burning";
  else if (g_state == STATE_MONITORING) stateStr = "monitoring";
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
  json += "}";
  server.send(200, "application/json", json);
}

void handleGetConfig() {
  String json = "{";
  json += "\"wifi_ssid\":\"" + jsonEscape(g_config.wifi_ssid) + "\"";
  json += ",\"wifi_password\":\"" + jsonEscape(g_config.wifi_password) + "\"";
  json += ",\"monitor_baud\":" + String(g_config.monitor_baud);
  json += ",\"verify\":" + String(g_config.verify ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handlePostConfig() {
  String body = server.arg("plain");
  // 合并保存：只更新请求中出现的字段
  String ssid = g_config.wifi_ssid;
  String pwd = g_config.wifi_password;
  uint32_t baud = g_config.monitor_baud;
  uint8_t verify = g_config.verify;
  // 通过查找 "key": 是否存在判断字段是否出现
  if (body.indexOf("\"wifi_ssid\"") >= 0)     ssid = jsonGetString(body, "wifi_ssid");
  if (body.indexOf("\"wifi_password\"") >= 0) pwd = jsonGetString(body, "wifi_password");
  if (body.indexOf("\"monitor_baud\"") >= 0)  baud = (uint32_t)jsonGetInt(body, "monitor_baud", 115200);
  if (body.indexOf("\"verify\"") >= 0)        verify = jsonGetBool(body, "verify", false) ? 1 : 0;
  saveConfig(ssid, pwd, baud, verify);
  g_config.wifi_ssid = ssid;
  g_config.wifi_password = pwd;
  g_config.monitor_baud = baud;
  g_config.verify = verify;
  server.send(200, "application/json", "{\"success\":true}");
}

void handleUpload() {
  HTTPUpload& upload = server.upload();
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
    if (g_upload_error) {
      server.send(500, "application/json", "{\"error\":\"write failed\"}");
    } else {
      String resp = "{\"success\":true,\"filename\":\"" + jsonEscape(g_upload_filename) +
                    "\",\"size\":" + String(upload.totalSize) + "}";
      server.send(200, "application/json", resp);
    }
  }
}

void handleBurn() {
  if (g_state != STATE_IDLE || g_burn_pending) {
    server.send(409, "application/json", "{\"error\":\"busy\"}");
    return;
  }
  String body = server.arg("plain");
  bool verify = jsonGetBool(body, "verify", false);
  String filename = jsonGetString(body, "filename");
  if (filename.length() == 0) filename = "firmware.bin";

  if (!LittleFS.exists("/" + filename)) {
    server.send(404, "application/json", "{\"error\":\"file not found\"}");
    return;
  }

  fs::File f = LittleFS.open("/" + filename, "r");
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
                ",\"total_blocks\":" + String(totalBlocks) + "}";
  server.send(202, "application/json", resp);
  Serial.printf("Burn queued: task=%u file=%s verify=%d blocks=%u\n",
                g_burn_task_id, filename.c_str(), verify, totalBlocks);
}

void handleMonitor() {
  if (g_state != STATE_IDLE) {
    server.send(409, "application/json", "{\"error\":\"busy\"}");
    return;
  }
  String body = server.arg("plain");
  uint32_t baud = (uint32_t)jsonGetInt(body, "baud", 115200);
  bool autoReset = jsonGetBool(body, "auto_reset", true);  // 默认自动复位
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
  bool wasMonitoring = (g_state == STATE_MONITORING);
  if (wasMonitoring) {
    // 推送剩余数据
    if (g_monitor_len > 0) pushMonitorData();
    g_monitor_len = 0;
    g_monitor_bytes_total = 0;
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
  // 临时堆分配在 ESP8266 ~50KB 堆上极易失败。默认 4KB，可下调
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
  String json = "{\"success\":true,\"files\":[";
  bool first = true;
  Dir dir = LittleFS.openDir("/");
  while (dir.next()) {
    String name = dir.fileName();
    // 跳过配置文件和 WebUI 静态文件
    if (name == "config.json" || name == "index.html" ||
        name == "style.css" || name == "app.js") continue;
    if (!first) json += ",";
    first = false;
    json += "{\"name\":\"" + jsonEscape(name) + "\"";
    json += ",\"size\":" + String(dir.fileSize());
    json += ",\"time\":" + String((uint32_t)dir.fileTime());
    json += "}";
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
    json += ",\"encrypted\":" + String(enc != ENC_TYPE_NONE ? "true" : "false");
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

void initHttpRoutes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/style.css", HTTP_GET, handleCss);
  server.on("/app.js", HTTP_GET, handleJs);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/config", HTTP_GET, handleGetConfig);
  server.on("/api/config", HTTP_POST, handlePostConfig);
  server.on("/api/upload", HTTP_POST, [](){ /* 响应在 handleUpload END 阶段发 */ }, handleUpload);
  server.on("/api/burn", HTTP_POST, handleBurn);
  server.on("/api/monitor", HTTP_POST, handleMonitor);
  server.on("/api/stop", HTTP_POST, handleStop);
  server.on("/api/reset", HTTP_POST, handleResetCC2530);
  server.on("/api/monitor/buffer", HTTP_GET, handleMonitorBuffer);
  server.on("/api/files", HTTP_GET, handleFiles);
  // /api/files/{name} - 用正则通配符
  server.on(UriRegex("^/api/files/([^/]+)$"), HTTP_DELETE, handleDeleteFile);
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
  server.collectHeaders("Accept", "User-Agent");
}

// ===== setup / loop =====
void setup() {
  ProgrammerInit();
  Serial.begin(115200);
  delay(100);
  Serial.println("\nCCLoader WebUI booting...");

  // 挂载 LittleFS
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed!");
    // 不 return，继续启动以便 AP 模式下可访问
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
  }
}
