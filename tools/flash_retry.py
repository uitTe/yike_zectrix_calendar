#!/usr/bin/env python
"""带「抓端口」重试的烧录器。

背景：设备跑的是**深睡探针固件**时，每轮唤醒后都会进 deep sleep，而 ESP32-S3 的
内置 USB-Serial/JTAG 外设在 deep sleep 下**断电** → COM 口周期性消失（实测约
3.6 s 在线 / 6 s 掉线一轮，周期 10 s）。此时直接跑 esptool 大概率撞在掉线窗口上。

做法（两阶段）：
  阶段 1「抓」—— 轮询等端口出现，立刻发 `--before default_reset --after no_reset`。
     一旦成功，芯片停在 ROM bootloader 里（bootloader **不会**深睡），USB 从此
     持续在线。
  阶段 2「烧」—— `--before no_reset` 直接连，写入 bootloader / 分区表 / 应用。

三个阶段整体可重试（外层 5 轮），单轮内「抓」最多试 15 次。

用法：
    tools/.venv/Scripts/python.exe tools/flash_retry.py [--port COM5] [--baud 921600]
"""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
import time

try:
    import serial.tools.list_ports as list_ports
except ImportError:  # pragma: no cover
    print("需要 pyserial（项目 venv 里已有）")
    raise SystemExit(2)

ROOT = pathlib.Path(__file__).resolve().parent.parent
ARGS_JSON = ROOT / "app" / "build" / "flasher_args.json"

GRAB_TRIES = 15        # 阶段 1 单轮内尝试次数
PORT_WAIT_S = 25.0     # 每次等待端口出现的上限
OUTER_TRIES = 5        # 整体重试轮数


def port_present(port: str) -> bool:
    return any(p.device == port for p in list_ports.comports())


def wait_port(port: str, timeout: float) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if port_present(port):
            return True
        time.sleep(0.04)
    return False


def load_flash_args() -> tuple[list[tuple[str, str]], dict]:
    if not ARGS_JSON.exists():
        print(f"找不到 {ARGS_JSON} —— 先跑一次构建")
        raise SystemExit(2)
    d = json.loads(ARGS_JSON.read_text(encoding="utf-8"))
    files = sorted(d["flash_files"].items(), key=lambda kv: int(kv[0], 16))
    out = []
    for off, rel in files:
        p = ROOT / "app" / "build" / rel
        if not p.exists():
            print(f"缺少构建产物：{p}")
            raise SystemExit(2)
        out.append((off, str(p)))
    return out, d["flash_settings"]


def run(cmd: list[str]) -> tuple[int, str]:
    r = subprocess.run(cmd, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    return r.returncode, (r.stdout or "") + (r.stderr or "")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM5")
    ap.add_argument("--baud", default="921600")
    a = ap.parse_args()

    files, settings = load_flash_args()
    base = [sys.executable, "-m", "esptool", "--chip", "esp32s3",
            "--port", a.port, "--baud", str(a.baud)]

    print(f"烧录目标 {a.port} @ {a.baud}，共 {len(files)} 个分段：")
    for off, f in files:
        print(f"  {off:10s} {pathlib.Path(f).name:<24s} {pathlib.Path(f).stat().st_size:>8,} B")
    print()

    for outer in range(1, OUTER_TRIES + 1):
        print(f"===== 第 {outer}/{OUTER_TRIES} 轮 =====")

        # ---------- 阶段 1：抓进 bootloader ----------
        print(f"[1/2] 等端口在线并 reset 进 bootloader（最多 {GRAB_TRIES} 次）")
        grabbed = False
        for i in range(1, GRAB_TRIES + 1):
            if not wait_port(a.port, PORT_WAIT_S):
                print(f"      #{i:<2d} {PORT_WAIT_S:.0f}s 内未出现端口")
                continue
            rc, out = run(base + ["--before", "default_reset",
                                  "--after", "no_reset", "flash_id"])
            if rc == 0 and "Chip is" in out:
                chip = next((ln.strip() for ln in out.splitlines()
                             if "Chip is" in ln), "?")
                print(f"      #{i:<2d} ✅ {chip}")
                grabbed = True
                break
            print(f"      #{i:<2d} ✗ 连接失败（端口掉线窗口），重试")
        if not grabbed:
            print("      本轮没抓到，进入下一轮\n")
            continue

        # ---------- 阶段 2：烧录 ----------
        print("[2/2] 写入分段（芯片已在 bootloader，端口保持在线）")
        cmd = base + ["--before", "no_reset", "--after", "hard_reset",
                      "write_flash",
                      "--flash_mode", settings["flash_mode"],
                      "--flash_freq", settings["flash_freq"],
                      "--flash_size", settings["flash_size"]]
        for off, f in files:
            cmd += [off, f]
        rc, out = run(cmd)

        for ln in out.splitlines():
            if any(k in ln for k in ("Writing at", "Wrote", "Hash of data verified",
                                     "Compressed", "Hard resetting", "A fatal error",
                                     "Failed", "ERROR")):
                print("      " + ln.strip())

        if rc == 0 and "Hash of data verified" in out:
            n = out.count("Hash of data verified")
            print(f"\n✅ 烧录成功 —— {n}/{len(files)} 段校验通过，设备已复位运行")
            return 0
        print("      本轮烧录失败，进入下一轮\n")

    print("\n❌ 多次重试仍未成功")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
