#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
CCLoader Agent 自动化调用示例（兼容新旧固件）

完整流程：
  1. 检查设备就绪
  2. 列出 LittleFS 中的 BIN
  3. 发起烧录（优先 async，旧固件自动降级同步）
  4. 跟踪烧录进度
  5. 启动监控 + 触发 CC2530 复位
  6. 通过 SSE 实时接收启动日志
  7. 停止监控

用法:
  python agent_demo.py [--ip 10.0.0.147] [--bin DIYRuZRT_256k.bin] [--no-verify] [--duration 15]
"""
import argparse
import base64
import json
import sys
import threading
import time

import requests

DEFAULT_IP = "10.0.0.147"
DEFAULT_BIN = "DIYRuZRT_256k.bin"
TIMEOUT = 5          # 普通 HTTP API 调用超时
SSE_READ_TIMEOUT = 8  # SSE 单次 read 超时（秒），无数据时也强制返回让主循环检查
SSE_HEARTBEAT_TIMEOUT = 30  # SSE 多少秒无任何数据视为设备卡死


def hr(t):
    print(f"\n=== {t} ===")


def check_device(ip):
    """检查设备就绪"""
    s = requests.get(f"http://{ip}/api/status", timeout=TIMEOUT).json()
    assert s["state"] == "idle", f"设备非 idle: {s['state']}"
    assert not s.get("burn_pending", False), "burn_pending=true"
    return s


def list_bin_files(ip):
    """列出 LittleFS 中的 BIN"""
    r = requests.get(f"http://{ip}/api/files", timeout=TIMEOUT).json()
    return [f["name"] for f in r.get("files", [])]


def burn_async(ip, filename, verify):
    """异步烧录（新固件）：返回 (task_id, total_blocks)。
    新固件已强制异步，旧固件降级时返回 None。"""
    try:
        r = requests.post(
            f"http://{ip}/api/burn",
            json={"filename": filename, "verify": verify},
            timeout=TIMEOUT,
        )
    except requests.exceptions.RequestException as e:
        print(f"  [burn 请求失败] {e}")
        return None
    if r.status_code == 202:
        d = r.json()
        return d.get("task_id"), d.get("total_blocks", 0)
    # 旧固件兼容：显式 ?async=1 探测
    try:
        r = requests.post(
            f"http://{ip}/api/burn?async=1",
            json={"filename": filename, "verify": verify},
            timeout=TIMEOUT,
        )
        if r.status_code == 202:
            d = r.json()
            return d.get("task_id"), d.get("total_blocks", 0)
    except requests.exceptions.RequestException:
        pass
    return None


def burn_sync(ip, filename, verify):
    """同步烧录（仅旧固件兼容路径，新固件不会走到这里）。
    同步烧录期间 HTTP 会被阻塞（256KB 约 90 秒），需用长 timeout。"""
    r = requests.post(
        f"http://{ip}/api/burn",
        json={"filename": filename, "verify": verify},
        timeout=300,  # 256KB 约 2 分钟
    )
    try:
        d = r.json()
    except Exception:
        d = {}
    return d.get("total_blocks", 0)


def wait_device_back(ip, deadline_s=240):
    """同步烧录后 HTTP 可能仍在阻塞，轮询直到设备响应。"""
    start = time.time()
    while time.time() - start < deadline_s:
        try:
            s = requests.get(f"http://{ip}/api/status", timeout=10).json()
            return s
        except Exception:
            time.sleep(5)
    return None


def poll_burn_progress(ip, deadline_s=240):
    """轮询烧录进度（异步模式）"""
    start = time.time()
    last_pct = -1
    while time.time() - start < deadline_s:
        try:
            s = requests.get(f"http://{ip}/api/status", timeout=TIMEOUT).json()
        except Exception as e:
            print(f"  [warn] 轮询失败: {e}")
            time.sleep(2)
            continue
        b = s.get("burn", {})
        pct = b.get("percent", 0)
        cur = b.get("current_block", 0)
        tot = b.get("total_blocks", 0)
        done = b.get("done", False)
        err = b.get("error", "")
        if pct != last_pct:
            print(f"  [{time.time()-start:5.1f}s] {pct:3d}%  {cur}/{tot}  err={err!r}")
            last_pct = pct
        if done:
            return err
        time.sleep(1)
    return "timeout"


def start_monitor(ip, baud=115200, auto_reset=True):
    """启动监控"""
    r = requests.post(
        f"http://{ip}/api/monitor",
        json={"baud": baud, "auto_reset": auto_reset},
        timeout=TIMEOUT,
    )
    return r.json()


def sse_capture(ip, duration_s, trigger_reset=True):
    """SSE 实时接收日志，可选触发复位。
    - 用 socket 超时避免设备卡死时 Python 永久挂起
    - 心跳检测：SSE_HEARTBEAT_TIMEOUT 秒无任何数据视为设备卡死，主动断开重连
    """
    received = bytearray()
    stop_flag = {"v": False}
    last_data_time = {"t": time.time()}

    def loop():
        import socket
        try:
            # 用 socket 超时控制 read，避免永久阻塞
            r = requests.get(f"http://{ip}:81/", stream=True, timeout=SSE_READ_TIMEOUT)
            # 设置底层 socket 超时
            r.raw._fp.fp.settimeout(SSE_READ_TIMEOUT)
            buf = b""
            for chunk in r.iter_content(chunk_size=128):
                if stop_flag["v"]:
                    break
                if not chunk:
                    continue
                last_data_time["t"] = time.time()
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    line = line.rstrip(b"\r")
                    if not line.startswith(b"data: "):
                        continue
                    try:
                        msg = json.loads(line[6:])
                    except Exception:
                        continue
                    t = msg.get("type")
                    if t == "monitor_data":
                        data = base64.b64decode(msg.get("data", ""))
                        sys.stdout.write(data.decode("utf-8", errors="replace"))
                        sys.stdout.flush()
                        received.extend(data)
                    elif t == "monitor_start":
                        print(f"[SSE] 监控开始 baud={msg.get('baud')}")
                    elif t == "monitor_reset":
                        print("[SSE] CC2530 已复位")
                    elif t == "monitor_stop":
                        print("[SSE] 监控停止")
                        stop_flag["v"] = True
                        break
            r.close()
        except requests.exceptions.RequestException as e:
            print(f"[SSE 网络错误] {e}")
        except socket.timeout:
            print(f"[SSE] {SSE_READ_TIMEOUT}s 无数据，断开（设备可能卡死）")
        except Exception as e:
            print(f"[SSE 错误] {e}")

    t = threading.Thread(target=loop, daemon=True)
    t.start()
    time.sleep(1)

    if trigger_reset:
        try:
            r = requests.post(f"http://{ip}/api/reset", timeout=TIMEOUT)
            print(f"  POST /api/reset -> HTTP {r.status_code}: {r.text}")
        except Exception as e:
            print(f"  [reset 失败] {e}")

    print(f"--- 等待 {duration_s}s 接收日志 ---")
    deadline = time.time() + duration_s
    while time.time() < deadline and not stop_flag["v"]:
        # 心跳检测：长时间无数据则主动停止
        if time.time() - last_data_time["t"] > SSE_HEARTBEAT_TIMEOUT:
            print(f"[SSE] {SSE_HEARTBEAT_TIMEOUT}s 心跳超时，主动断开")
            stop_flag["v"] = True
            break
        time.sleep(0.5)
    stop_flag["v"] = True
    t.join(timeout=3)
    return bytes(received)


def stop_monitor(ip):
    """停止监控"""
    try:
        r = requests.post(f"http://{ip}/api/stop", timeout=TIMEOUT)
        return r.json()
    except Exception as e:
        return {"error": str(e)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", default=DEFAULT_IP)
    ap.add_argument("--bin", default=DEFAULT_BIN)
    ap.add_argument("--verify", action="store_true", default=True)
    ap.add_argument("--no-verify", dest="verify", action="store_false")
    ap.add_argument("--duration", type=int, default=15, help="监控持续时间(秒)")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    ip = args.ip
    filename = args.bin
    verify = args.verify

    # 1. 设备就绪
    hr("设备状态")
    try:
        s = check_device(ip)
        print(f"  state={s['state']} ip={s['wifi']['ip']} uptime={s['uptime']}s")
    except Exception as e:
        print(f"[ERROR] {e}")
        sys.exit(1)

    # 2. 列出 BIN
    hr("LittleFS 文件")
    files = list_bin_files(ip)
    print(f"  {files}")
    if filename not in files:
        print(f"[ERROR] LittleFS 中没有 {filename}")
        sys.exit(1)

    # 3. 发起烧录（优先 async）
    hr(f"发起烧录 {filename} (verify={verify})")
    task_info = burn_async(ip, filename, verify)
    if task_info:
        task_id, total_blocks = task_info
        print(f"  [新固件 async] task_id={task_id} total_blocks={total_blocks}")
        err = poll_burn_progress(ip)
    else:
        print(f"  [旧固件 sync] 阻塞烧录中（约 90s）...")
        total_blocks = burn_sync(ip, filename, verify)
        print(f"  HTTP 返回 total_blocks={total_blocks}")
        # 旧固件可能立即响应但烧录在后台执行
        # 等待设备恢复响应并轮询进度
        print(f"  等待设备恢复响应...")
        s = wait_device_back(ip, deadline_s=120)
        if s is None:
            print("[FAIL] 设备无响应")
            sys.exit(1)
        # 旧固件后台烧录：轮询直到 done
        if s["state"] == "burning" or not s.get("burn", {}).get("done"):
            print(f"  设备状态: state={s['state']} burn={s.get('burn')}")
            print(f"  轮询烧录进度...")
            err = poll_burn_progress(ip, deadline_s=240)
        else:
            b = s.get("burn", {})
            err = b.get("error", "") if b.get("done") else "not done"
            print(f"  设备恢复: state={s['state']} burn.done={b.get('done')} burn.error={err!r}")

    if err:
        print(f"[FAIL] 烧录失败: {err}")
        sys.exit(1)
    print("[OK] 烧录完成")

    # 4. 启动监控
    hr(f"启动监控 baud={args.baud}")
    r = start_monitor(ip, baud=args.baud, auto_reset=False)  # 自己触发 reset
    print(f"  {r}")

    # 5. SSE 捕获日志
    hr("SSE 实时日志")
    data = sse_capture(ip, duration_s=args.duration, trigger_reset=True)
    print(f"\n--- 共接收 {len(data)} 字节 ---")
    if data:
        print("HEX:", data.hex())

    # 6. 停止监控
    hr("停止监控")
    print(f"  {stop_monitor(ip)}")

    hr("全部完成")


if __name__ == "__main__":
    main()
