#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
cc_monitor.py — 通过 ESP8266 (CCLoader 改造版) 监控 CC2530 的 UART0 日志

工作流：
  1. 以 115200 打开 ESP8266 所在 COM 口
  2. 发 CMD_PING 探活，确认 ESP8266 在 IDLE 态
  3. 发 CMD_MONITOR_BEGIN + 4 字节波特率，进入监控模式
  4. ESP8266 与本脚本同步切换到目标波特率
  5. 循环解析 RSP_MONITOR_DATA 帧，把 DATA 写入控制台/文件
  6. Ctrl+C 退出时发 CMD_MONITOR_STOP，并恢复 115200

用法：
  python cc_monitor.py COM4 115200
  python cc_monitor.py --port COM4 --baud 115200 --timeout 600 --output log.txt --timestamp

依赖：pyserial >= 3.5
"""

import argparse
import struct
import sys
import time

try:
    import serial
except ImportError:
    sys.stderr.write("缺少 pyserial，请先执行: pip install pyserial\n")
    sys.exit(1)


# ===== 协议常量（与 src/CCLoader.ino 保持一致）=====
CMD_PING             = 0xFF
CMD_MONITOR_BEGIN    = 0x10
CMD_MONITOR_STOP     = 0x11

RSP_OK               = 0x01
RSP_ERROR            = 0x02
RSP_MONITOR_DATA     = 0xA0
RSP_MONITOR_END      = 0xA1

DEFAULT_BAUD         = 115200   # ESP8266 空闲/烧录态波特率
SYNC_WAIT_S          = 0.2      # 波特率切换后等 ESP8266 稳定的时间
PING_TIMEOUT_S       = 1.0      # PING 响应超时


def crc8(data: bytes) -> int:
    """CRC8：多项式 0x07，初值 0x00，仅对 DATA 计算（与固件 crc8() 一致）"""
    crc = 0x00
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x07) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc


def _read_exact(ser, n: int, timeout_s: float) -> bytes:
    """从串口精确读 n 字节，超时返回已读到的部分"""
    buf = bytearray()
    deadline = time.time() + timeout_s
    while len(buf) < n:
        remaining = n - len(buf)
        chunk = ser.read(remaining)
        if chunk:
            buf.extend(chunk)
        if time.time() > deadline:
            break
    return bytes(buf)


def _send_ping(ser) -> bool:
    """发 PING 并等待 RSP_OK，超时返回 False"""
    ser.reset_input_buffer()
    ser.write(bytes([CMD_PING]))
    rsp = _read_exact(ser, 1, PING_TIMEOUT_S)
    return rsp == bytes([RSP_OK])


def _enter_monitor(ser, baud: int) -> bool:
    """发 CMD_MONITOR_BEGIN + 4 字节波特率，切换波特率，等 RSP_OK

    时序：本端发完命令后尽快切换 baudrate（~100ms），ESP8266 端 delay(300ms)
    后才用新波特率发 RSP_OK，留 200ms 余量避免波特率错配读成乱码。
    """
    ser.write(bytes([CMD_MONITOR_BEGIN]) + struct.pack('<I', baud))
    # 尽快切换到目标波特率（必须在 ESP8266 发 RSP_OK 之前完成）
    time.sleep(0.1)
    ser.baudrate = baud
    # 清空切换期间可能累积的乱码字节，确保后续读到的是干净的 RSP_OK
    ser.reset_input_buffer()
    # ESP8266 用新波特率发 RSP_OK
    rsp = _read_exact(ser, 1, 1.0)
    return rsp == bytes([RSP_OK])


def _stop_monitor(ser) -> None:
    """发 CMD_MONITOR_STOP（先切回默认波特率，因为 ESP8266 退出后会恢复 115200）"""
    try:
        ser.baudrate = DEFAULT_BAUD
        time.sleep(0.1)
        ser.write(bytes([CMD_MONITOR_STOP]))
        # 等 ESP8266 回 RSP_MONITOR_END（不强制要求）
        _read_exact(ser, 1, 0.5)
    except Exception:
        pass


def _emit_line(line: bytes, timestamp: bool, outfile) -> None:
    """输出一行日志到控制台和可选文件"""
    if timestamp:
        now = time.time()
        ts = time.strftime('%H:%M:%S') + '.{:03d}'.format(int((now % 1) * 1000))
        sys.stdout.buffer.write(b'[' + ts.encode('ascii') + b'] ' + line)
    else:
        sys.stdout.buffer.write(line)
    sys.stdout.buffer.flush()
    if outfile:
        outfile.write(line)
        outfile.flush()


def monitor_loop(ser, timeout_s: int, timestamp: bool, outfile) -> None:
    """监控主循环：解析 RSP_MONITOR_DATA 帧，收到 RSP_MONITOR_END 退出"""
    start_time = time.time()
    line_buf = bytearray()

    while timeout_s <= 0 or time.time() - start_time < timeout_s:
        head = _read_exact(ser, 1, 0.5)
        if not head:
            continue
        ch = head[0]

        if ch == RSP_MONITOR_DATA:
            # 读 LEN_HI, LEN_LO
            len_bytes = _read_exact(ser, 2, 1.0)
            if len(len_bytes) < 2:
                sys.stderr.write("[警告] 帧头 LEN 不完整，丢弃\n")
                continue
            data_len = (len_bytes[0] << 8) | len_bytes[1]
            if data_len == 0 or data_len > 64:
                sys.stderr.write("[警告] LEN 异常: %d，丢弃\n" % data_len)
                continue
            # 读 DATA + CRC8
            payload = _read_exact(ser, data_len + 1, 2.0)
            if len(payload) < data_len + 1:
                sys.stderr.write("[警告] DATA 不完整，期望 %d 字节，实际 %d\n"
                                 % (data_len + 1, len(payload)))
                continue
            data = payload[:data_len]
            crc_recv = payload[data_len]
            # CRC 校验
            if crc8(data) != crc_recv:
                sys.stderr.write("[CRC 错误] 丢弃 %d 字节\n" % data_len)
                continue
            # 按行输出（保留不完整行在 line_buf 中等下一帧）
            line_buf.extend(data)
            while True:
                idx = line_buf.find(b'\n')
                if idx < 0:
                    break
                line = bytes(line_buf[:idx + 1])
                del line_buf[:idx + 1]
                _emit_line(line, timestamp, outfile)
            # 无换行符的数据也实时输出（避免长时间无 \n 时屏幕卡住）
            if line_buf and len(line_buf) >= 128:
                _emit_line(bytes(line_buf), timestamp, outfile)
                line_buf.clear()

        elif ch == RSP_MONITOR_END:
            print("\n[ESP8266] 退出监控模式")
            break
        else:
            # 非帧头字节，可能是上位机和 ESP8266 波特率未对齐，丢弃并提示
            sys.stderr.write("[警告] 意外字节 0x%02X\n" % ch)


def main():
    parser = argparse.ArgumentParser(
        description='通过 ESP8266 (CCLoader 改造版) 监控 CC2530 串口日志')
    parser.add_argument('port', nargs='?', help='ESP8266 所在 COM 口，如 COM4')
    parser.add_argument('baud', nargs='?', type=int, default=DEFAULT_BAUD,
                        help='CC2530 UART 波特率，默认 %d' % DEFAULT_BAUD)
    parser.add_argument('--port', dest='port_opt', help='同位置参数 port')
    parser.add_argument('--baud', dest='baud_opt', type=int,
                        help='同位置参数 baud')
    parser.add_argument('--timeout', type=int, default=600,
                        help='监控时长（秒），默认 600，0 表示不限制')
    parser.add_argument('--output', help='日志输出文件（可选）')
    parser.add_argument('--timestamp', action='store_true',
                        help='每行加 [HH:MM:SS.mmm] 时间戳')
    args = parser.parse_args()

    port = args.port or args.port_opt
    baud = args.baud_opt or args.baud
    if not port:
        parser.error('必须指定 COM 口，例如: python cc_monitor.py COM4 115200')
    if baud < 9600 or baud > 230400:
        parser.error('波特率必须在 9600~230400 之间')

    ser = serial.Serial(port, DEFAULT_BAUD, timeout=0.5)
    outfile = open(args.output, 'wb') if args.output else None

    try:
        # 1. PING 探活
        print("[1/3] 探活 ESP8266 (%s @ %d)..." % (port, DEFAULT_BAUD))
        if not _send_ping(ser):
            print("ESP8266 未响应 PING，可能不在线或处于烧录态")
            return
        print("      ESP8266 在线，IDLE 态")

        # 2. 进入监控模式
        print("[2/3] 进入监控模式，切换波特率到 %d..." % baud)
        if not _enter_monitor(ser, baud):
            print("进入监控模式失败（ESP8266 未回 RSP_OK）")
            return
        print("      已进入监控模式")

        # 3. 监控循环
        print("[3/3] 监控中... 按 Ctrl+C 退出")
        print("-" * 60)
        monitor_loop(ser, args.timeout, args.timestamp, outfile)

    except KeyboardInterrupt:
        print("\n[用户中断] 发送 STOP...")
        _stop_monitor(ser)
        print("已退出监控模式")
    finally:
        if outfile:
            outfile.close()
        try:
            ser.baudrate = DEFAULT_BAUD
        except Exception:
            pass
        ser.close()


if __name__ == '__main__':
    main()
