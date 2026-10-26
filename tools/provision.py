"""通过 USB Serial/JTAG 向 Zectrix Note 4 写入配置。

设备只在配网模式下响应：首次开机 NVS 里没有凭据，或长按 UP 键按住到 8 秒重启。
写入成功后设备会立即试连并重启。

两块内容可以只写一块：
    WiFi      --ssid/--pass
    取数凭证  --agc-client-id/--agc-secret/--agc-api-key/--agc-group-id
留空的字段不会改动 NVS 里已存的值。要自己拼完整 JSON 时用 --json。
"""

import argparse
import json
import pathlib
import sys
import time

import serial
from serial.tools import list_ports

REPLY_PREFIX = "PROV "


def build_payload(args) -> str:
    """按参数拼请求体；不属于任何一块的参数组合直接报错，不猜。"""
    if args.json:
        return args.json
    if args.json_file:
        return pathlib.Path(args.json_file).read_text(encoding="utf-8").strip()
    payload = {}
    if args.ssid:
        payload["wifi"] = {"ssid": args.ssid, "pass": args.password}
    agc = {
        "clientId": args.agc_client_id,
        "secret": args.agc_secret,
        "apiKey": args.agc_api_key,
        "groupId": args.agc_group_id,
    }
    if any(agc.values()):
        payload["agc"] = {k: v for k, v in agc.items() if v}
    return json.dumps(payload, ensure_ascii=False) if payload else ""


def describe_ports() -> None:
    ports = sorted(list_ports.comports(), key=lambda item: item.device)
    if not ports:
        print("未找到串口设备", file=sys.stderr)
        return
    for port in ports:
        print(f"{port.device}\t{port.description}")


def provision(port: str, payload: str, timeout: float) -> int:
    try:
        with serial.Serial(port, 115200, timeout=0.2) as link:
            # 不要碰 DTR：ESP32-S3 的 USB-Serial/JTAG 把 DTR 映射到 EN 引脚，
            # 拉低产生的下降沿等于按一下复位，设备会在脚本写入之前重启。
            # 打开串口后保持默认电平即可，RTS 也不动，避免误入下载模式。
            time.sleep(0.3)
            link.reset_input_buffer()
            link.write((payload + "\n").encode("utf-8"))
            link.flush()

            deadline = time.time() + timeout
            while time.time() < deadline:
                raw = link.readline()
                if not raw:
                    continue
                text = raw.decode("utf-8", "replace").strip()
                if not text.startswith(REPLY_PREFIX):
                    continue
                try:
                    reply = json.loads(text[len(REPLY_PREFIX):])
                except json.JSONDecodeError:
                    print(f"回执不是合法 JSON：{text}", file=sys.stderr)
                    return 1
                print(json.dumps(reply, ensure_ascii=False, indent=2))
                return 0 if reply.get("ok") else 1
    except serial.SerialException as error:
        # 配网窗口结束时设备会渲染页面并进入深睡，USB-Serial/JTAG 随之断电，
        # 串口在读写过程中直接消失。这属于正常时序，不是脚本故障。
        print(f"串口中断：{error}", file=sys.stderr)
        print("设备已离开配网窗口或进入深睡。长按上键 6 秒可重新进入配网。", file=sys.stderr)
        return 1

    print(
        f"{timeout:.0f} 秒内未收到回执。确认设备处于配网模式，且串口 {port} 未被占用。",
        file=sys.stderr,
    )
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description="向 Zectrix Note 4 写入 WiFi 与取数凭证")
    parser.add_argument("--port", help="串口设备，例如 COM5")
    parser.add_argument("--ssid", help="WiFi 名称")
    parser.add_argument("--pass", dest="password", default="", help="WiFi 密码，开放网络留空")
    parser.add_argument("--agc-client-id", help="AGC Client ID")
    parser.add_argument("--agc-secret", help="AGC Client Secret")
    parser.add_argument("--agc-api-key", help="云函数 apiKey")
    parser.add_argument("--agc-group-id", help="分组编号 groupId")
    parser.add_argument("--json", help="直传完整请求体，优先于其它参数")
    parser.add_argument("--json-file",
                        help="从文件读请求体（避免密钥留在命令行历史里），优先级次之")
    parser.add_argument("--timeout", type=float, default=30.0, help="等待回执的秒数")
    parser.add_argument("--list", action="store_true", help="列出本机串口")
    args = parser.parse_args()

    if args.list:
        describe_ports()
        return 0
    if not args.port:
        parser.error("需要 --port")
    payload = build_payload(args)
    if not payload or payload == "{}":
        parser.error("要么给 --ssid，要么给至少一项 --agc-*，要么用 --json")

    return provision(args.port, payload, args.timeout)


if __name__ == "__main__":
    sys.exit(main())
