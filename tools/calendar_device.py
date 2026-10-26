"""真实设备的串口捕获、RTC 校时和零点唤醒验证。"""
from __future__ import annotations

import argparse
import datetime as dt
import json
from pathlib import Path
import re
import subprocess
import sys
import time

import serial
from serial.tools import list_ports
import calendar_A_final as calendar
import lunar_cn

ROOT = Path(__file__).resolve().parent.parent
LOG_DIR = ROOT / ".tmp_device"


def find_port():
    ports = [p.device for p in list_ports.comports() if p.vid == 0x303A]
    if len(ports) > 1:
        raise RuntimeError(f"发现多个 ESP32 设备：{ports}")
    return ports[0] if ports else None


def open_port(port):
    connection = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    connection.dtr = False
    connection.rts = False
    connection.port = port
    connection.open()
    return connection


class Device:
    def __init__(self, seconds):
        LOG_DIR.mkdir(exist_ok=True)
        path = LOG_DIR / f"rtc_{dt.datetime.now():%Y%m%d-%H%M%S}.log"
        self.log = path.open("w", encoding="utf-8")
        print(f"log={path}", flush=True)
        self.deadline = time.monotonic() + seconds
        self.connection = None
        self.pending = bytearray()

    def close(self):
        if self.connection is not None:
            self.connection.close()
            self.connection = None
        self.log.close()

    def next_line(self):
        while time.monotonic() < self.deadline:
            if b"\n" in self.pending:
                raw, _, remaining = self.pending.partition(b"\n")
                self.pending = bytearray(remaining)
                line = raw.decode("utf-8", errors="replace").rstrip("\r")
                print(line, flush=True)
                self.log.write(line + "\n")
                self.log.flush()
                return line
            if self.connection is None:
                port = find_port()
                if port is None:
                    time.sleep(0.04)
                    continue
                try:
                    self.connection = open_port(port)
                except serial.SerialException:
                    time.sleep(0.04)
                    continue
            try:
                chunk = self.connection.read(min(self.connection.in_waiting, 4096) or 1)
                self.pending.extend(chunk)
            except serial.SerialException:
                self.connection.close()
                self.connection = None
                self.pending.clear()
        raise TimeoutError("等待设备串口或运行日志超时")

    def wait(self, text):
        while True:
            line = self.next_line()
            if "Guru Meditation" in line or "ESP_ERROR_CHECK failed" in line:
                raise RuntimeError(line)
            if "mismatch" in line and "0 mismatch" not in line:
                raise RuntimeError(line)
            if text in line:
                return line

    def send(self, request):
        assert self.connection is not None
        self.pending.clear()
        self.connection.reset_input_buffer()
        self.connection.write(json.dumps(request).encode("ascii") + b"\n")
        self.connection.flush()

def grab(seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        port = find_port()
        if port is None:
            time.sleep(0.04)
            continue
        print(f"检测到 {port}，请求进入 ROM 下载模式", flush=True)
        result = subprocess.run(
            [sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", port,
             "--before", "default_reset", "--after", "no_reset", "flash_id"],
            capture_output=True, text=True, encoding="utf-8", errors="replace")
        print(result.stdout, flush=True)
        if result.returncode == 0:
            print(f"下载模式已保持：{port}", flush=True)
            return
        print(result.stderr, flush=True)
    raise TimeoutError("未捕获到 USB 串口，请让设备进入下载模式")


def verify_page(device, date):
    line = device.wait(f"page {date.year}年{date.month}月{date.day}日")
    info = lunar_cn.day_info(date)
    term, festivals = calendar.badges(date)
    assert f"{info['weekday_cn']}  农历 {info['lunar_cn']}" in line, line
    assert f"节气 {term or '-'}  节日 {'+'.join(festivals) or '-'}" in line, line
    assert "班次留空" in line, line


def midnight_test(device):
    for before in (dt.datetime(2026, 9, 24, 23, 59, 35),
                   dt.datetime(2026, 9, 30, 23, 59, 35),
                   dt.datetime(2026, 12, 31, 23, 59, 35)):
        after = before.date() + dt.timedelta(days=1)
        device.wait("rtc_console: READY")
        device.send({"time": before.isoformat(), "run": True})
        verify_page(device, before.date())
        device.wait(f"armed until {after.isoformat()} 00:00:00")
        device.wait("rtc_console: READY")
        device.send({"run": True})
        device.wait(f"deep sleep until {after.isoformat()} 00:00:00")
        ready = device.wait("rtc_console: READY")
        match = re.search(r"cause=(\d+) ext1=0x([0-9a-fA-F]+) time=(\S+)", ready)
        assert match is not None, ready
        assert int(match[1]) == 3 and int(match[2], 16) == 1 << 5, ready
        actual = dt.datetime.fromisoformat(match[3])
        assert actual.date() == after and actual.hour == actual.minute == 0, ready
        assert actual.second < 15, ready
        device.send({"run": True})
        verify_page(device, after)
        device.wait(f"armed until {(after + dt.timedelta(days=1)).isoformat()} 00:00:00")
        device.wait("rtc_console: READY")
        print(f"PASS: {before.date()} -> {after}, RTC GPIO5 wake and daily rearm", flush=True)
        device.send({"restart": True})


def button_test(device):
    today = dt.datetime.now(dt.timezone(dt.timedelta(hours=8))).date()
    verify_page(device, today)
    device.wait(f"deep sleep until {(today + dt.timedelta(days=1)).isoformat()} 00:00:00")
    print("STEP: press BOOT briefly, then hold UP for 3 seconds and release", flush=True)
    wake = device.wait("calendar: wake cause=3 ext1=0x1 ")
    assert "reset_reason=8" in wake, wake
    verify_page(device, today)
    device.wait("BOOT wake: awake for 30 seconds")
    device.wait("UP long press released: restart")
    reset = device.wait("calendar: wake cause=0 ext1=0x0 reset_reason=3")
    assert "reset_reason=3" in reset, reset
    verify_page(device, today)
    device.wait(f"deep sleep until {(today + dt.timedelta(days=1)).isoformat()} 00:00:00")
    print("PASS: real BOOT wake, UP long-press software reset, daily alarm restored", flush=True)


def idle_test(device):
    today = dt.datetime.now(dt.timezone(dt.timedelta(hours=8))).date()
    device.wait("calendar: wake cause=3 ext1=0x1 ")
    verify_page(device, today)
    ready = device.wait("BOOT wake: awake for 30 seconds")
    started_ms = int(re.search(r"I \((\d+)\)", ready)[1])
    sleep = device.wait(f"deep sleep until {(today + dt.timedelta(days=1)).isoformat()} 00:00:00")
    finished_ms = int(re.search(r"I \((\d+)\)", sleep)[1])
    assert 30000 <= finished_ms - started_ms < 35000, sleep
    device.connection.close()
    device.connection = None
    time.sleep(1)
    for _ in range(30):
        assert find_port() is None, "BOOT 松开后出现重复唤醒"
        time.sleep(0.1)
    print(f"PASS: BOOT idle sleep after {finished_ms - started_ms} ms, no repeat wake", flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=("grab", "monitor", "test", "sync", "verify", "buttons", "idle"))
    parser.add_argument("--seconds", type=int, default=120)
    args = parser.parse_args()
    if args.action == "grab":
        grab(args.seconds)
        return
    device = Device(args.seconds)
    try:
        if args.action == "test":
            midnight_test(device)
        elif args.action == "buttons":
            button_test(device)
        elif args.action == "idle":
            idle_test(device)
        elif args.action == "sync":
            device.wait("rtc_console: READY")
            now = dt.datetime.now(dt.timezone(dt.timedelta(hours=8))).replace(tzinfo=None, microsecond=0)
            device.send({"time": now.isoformat()})
            device.wait(f"SET time={now.isoformat()}")
            ready = device.wait("rtc_console: READY")
            current = dt.datetime.fromisoformat(ready.split("time=", 1)[1])
            assert abs((current - now).total_seconds()) < 5, ready
            print("RTC 已恢复当前北京时间，校时入口保持可用", flush=True)
        elif args.action == "verify":
            today = dt.datetime.now(dt.timezone(dt.timedelta(hours=8))).date()
            verify_page(device, today)
            device.wait(f"deep sleep until {(today + dt.timedelta(days=1)).isoformat()} 00:00:00")
            device.connection.close()
            device.connection = None
            time.sleep(1)
            for _ in range(30):
                assert find_port() is None, "设备未保持深度休眠"
                time.sleep(0.1)
            print("正式固件当天页面、下一次零点闹钟与持续深度休眠验证通过", flush=True)
        else:
            while True:
                device.next_line()
    finally:
        device.close()


if __name__ == "__main__":
    main()
