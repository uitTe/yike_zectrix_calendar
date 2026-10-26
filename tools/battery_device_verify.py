"""复位真实设备，验证电量、当天页面与深度休眠。"""
from __future__ import annotations

import datetime as dt
import hashlib
import json
from pathlib import Path
import re
import time

from esptool.reset import HardReset
from calendar_device import Device, find_port, open_port, verify_page

ROOT = Path(__file__).resolve().parent.parent


def main():
    port = find_port()
    if port is None:
        raise RuntimeError("设备 USB 串口未出现")
    today = dt.datetime.now(dt.timezone(dt.timedelta(hours=8))).date()
    device = Device(55)
    try:
        device.connection = open_port(port)
        HardReset(device.connection, uses_usb=True)()
        device.wait("render self-check: layout OK, 9 cases, 0 mismatch")
        line = device.wait("calendar: battery ")
        match = re.search(r"battery (\d+) mV raw=(\d+)% state=(\d+)", line)
        if match is None:
            raise RuntimeError(f"电池日志字段无效：{line}")
        millivolts, percent, state = map(int, match.groups())
        assert 2000 <= millivolts <= 5000, line
        assert 0 <= percent <= 100 and 0 <= state <= 4, line
        verify_page(device, today)
        sleep = device.wait(f"deep sleep until {(today + dt.timedelta(days=1)).isoformat()} 00:00:00")
        device.connection.close()
        device.connection = None
        time.sleep(1)
        for _ in range(30):
            assert find_port() is None, "设备未保持深度休眠"
            time.sleep(0.1)
        result = {
            "verified_at": dt.datetime.now(dt.timezone(dt.timedelta(hours=8))).isoformat(),
            "port": port,
            "battery_mv": millivolts,
            "battery_percent": percent,
            "display_percent": 100 if state == 2 else percent // 5 * 5,
            "battery_state": state,
            "date": today.isoformat(),
            "sleep": sleep,
            "log": device.log.name,
            "firmware_sha256": hashlib.sha256((ROOT / "app/build/zectrix_calendar.bin").read_bytes()).hexdigest(),
            "ok": True,
        }
        output = ROOT / ".tmp_device/battery_verification.json"
        output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(result, ensure_ascii=False, indent=2), flush=True)
    finally:
        device.close()


if __name__ == "__main__":
    main()
