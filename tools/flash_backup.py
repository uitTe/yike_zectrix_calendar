#!/usr/bin/env python3
"""Zectrix Note 4 原厂 Flash 备份 / 校验 / 回写工具。

这是刷任何第三方固件之前的**唯一退路**。ESP32-S3 的 ROM bootloader 固化在芯片内部，
固件刷坏可以重新进下载模式重刷；但**恢复"出厂原样"（含 Wi-Fi 配置与射频校准数据）
只能靠自己这份 16 MiB 全量备份**。

用法
----
    # 备份（设备先按 BOOT 再插 USB 进入下载模式）
    python flash_backup.py backup --port COM5

    # 串口自动探测
    python flash_backup.py backup

    # 校验已有备份（不接设备）
    python flash_backup.py verify backup/note4_flash_20260922-155500.bin

    # 空转测试：把备份原样写回设备，验证整条恢复链路可用
    python flash_backup.py restore backup/note4_flash_20260922-155500.bin --port COM5

自检三要素（verify 会全部检查）
-------------------------------
    1. 文件大小      == 16777216 字节（16 MiB）
    2. 偏移 0x0      == 0xE9            （ESP 镜像魔术字节）
    3. 偏移 0x8000   == AA 50           （分区表魔术字节）
    附加：偏移 0xC   == 0x09            （芯片 ID = ESP32-S3）

红线
----
* 绝不开启 Secure Boot / Flash Encryption —— 会烧 eFuse，永久无法退回原厂。
* 写入期间绝不拔线 / 断电 / 让电脑休眠；用数据线直连，不要过 USB HUB。
* 绝不要用别人设备的备份 —— eFuse MAC 不同、射频校准逐台测试、可能带入他人凭据。
"""

from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path

FLASH_SIZE = 16 * 1024 * 1024  # 16 MiB，Zectrix Note 4 = ESP32-S3 N16R8
MAGIC_IMAGE = 0xE9            # ESP 镜像首字节
PART_TABLE_OFFSET = 0x8000    # 分区表偏移
MAGIC_PARTITION = b"\xAA\x50"  # 分区表魔术字节
CHIP_ID_OFFSET = 0x0C         # 镜像头里的芯片 ID
CHIP_ID_ESP32S3 = 0x09

PROJECT_ROOT = Path(__file__).resolve().parent.parent
BACKUP_DIR = PROJECT_ROOT / "backup"


# --------------------------------------------------------------------------- #
# 基础设施
# --------------------------------------------------------------------------- #
def _esptool(args: list[str]) -> int:
    """以子进程方式调用 esptool，继承当前 venv 的解释器。"""
    cmd = [sys.executable, "-m", "esptool", *args]
    print(f"\n$ {' '.join(cmd)}\n", flush=True)
    return subprocess.call(cmd)


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def _autodetect_port() -> str | None:
    """扫描串口，优先返回看起来像 USB 转串口的设备。"""
    try:
        from serial.tools import list_ports  # esptool 依赖 pyserial，必然可用
    except ImportError:
        return None

    candidates = []
    for p in list_ports.comports():
        desc = f"{p.description or ''} {p.hwid or ''}".lower()
        # 排除蓝牙虚拟串口；ESP32-S3 原生 USB 常见描述见下
        if "bluetooth" in desc or "蓝牙" in desc:
            continue
        score = 0
        for kw in ("usb", "jtag", "serial", "cp210", "ch340", "ch9102", "ftdi"):
            if kw in desc:
                score += 1
        candidates.append((score, p.device, p.description))

    if not candidates:
        return None
    candidates.sort(key=lambda t: -t[0])
    print("检测到候选串口：")
    for score, dev, desc in candidates:
        print(f"  {dev:8s}  {desc}")
    print(f"→ 选用 {candidates[0][1]}\n")
    return candidates[0][1]


# --------------------------------------------------------------------------- #
# 校验
# --------------------------------------------------------------------------- #
def check_image(path: Path, *, expect_full: bool = True) -> dict:
    """对镜像做结构自检，返回结果字典（不改动文件）。"""
    res: dict = {"file": str(path), "exists": path.exists()}
    if not path.exists():
        return res

    size = path.stat().st_size
    res["size"] = size
    res["sha256"] = _sha256(path)

    with path.open("rb") as fh:
        head = fh.read(PART_TABLE_OFFSET + 16)

    res["byte_0x0"] = head[0] if len(head) > 0 else None
    res["chip_id_0xc"] = head[CHIP_ID_OFFSET] if len(head) > CHIP_ID_OFFSET else None
    res["partition_magic"] = head[PART_TABLE_OFFSET:PART_TABLE_OFFSET + 2].hex().upper()

    checks = {
        "大小 = 16777216 字节": size == FLASH_SIZE,
        "偏移 0x0 = 0xE9（ESP 镜像）": res["byte_0x0"] == MAGIC_IMAGE,
        "偏移 0x8000 = AA50（分区表）":
            head[PART_TABLE_OFFSET:PART_TABLE_OFFSET + 2] == MAGIC_PARTITION,
        "偏移 0xC = 0x09（ESP32-S3）": res["chip_id_0xc"] == CHIP_ID_ESP32S3,
    }
    if not expect_full:
        # 校验分区/应用镜像时不做全尺寸要求
        checks.pop("大小 = 16777216 字节")

    res["checks"] = checks
    res["ok"] = all(checks.values())
    return res


def print_report(res: dict) -> None:
    print("=" * 68)
    print("备份自检报告")
    print("=" * 68)
    for key in ("file", "size", "sha256", "byte_0x0", "chip_id_0xc", "partition_magic"):
        if key in res:
            val = res[key]
            if isinstance(val, int):
                val = hex(val)
            print(f"  {key:18s} : {val}")
    print("-" * 68)
    for name, ok in res.get("checks", {}).items():
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}")
    print("=" * 68)
    print("✅ 备份有效，可作为恢复出厂原样的退路。" if res.get("ok")
          else "❌ 备份未通过自检 —— 不要依赖它，请重新备份！")
    print("=" * 68)


# --------------------------------------------------------------------------- #
# 子命令
# --------------------------------------------------------------------------- #
def _esptool_base(port: str, baud: int, no_stub: bool) -> list[str]:
    args = ["--port", port, "--baud", str(baud)]
    if no_stub:
        args += ["--no-stub"]
    return args


def _read_chunk(port: str, baud: int, offset: int, size: int,
                dest: Path, *, no_stub: bool, retries: int) -> bool:
    """读取一段 Flash 到独立文件，失败自动重试。成功返回 True。"""
    for attempt in range(1, retries + 1):
        if dest.exists():
            dest.unlink()
        rc = _esptool([
            *_esptool_base(port, baud, no_stub),
            "--before", "default_reset", "--after", "no_reset",
            "read_flash", hex(offset), hex(size), str(dest),
        ])
        if rc == 0 and dest.exists() and dest.stat().st_size == size:
            return True
        print(f"    ⚠️  第 {attempt}/{retries} 次读取 0x{offset:X} 失败"
              f"（{'退出码 ' + str(rc) if rc else '文件长度不符'}）"
              + ("，重试…" if attempt < retries else "，放弃。"))
        if attempt < retries:
            time.sleep(1.5)
    return False


def cmd_backup(args: argparse.Namespace) -> int:
    port = args.port or _autodetect_port()
    if not port:
        print("❌ 未找到可用串口。请确认设备已进入下载模式（按住 BOOT 再插 USB），"
              "然后用 --port COMx 显式指定。")
        return 2

    BACKUP_DIR.mkdir(parents=True, exist_ok=True)
    stamp = _dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = Path(args.output) if args.output else BACKUP_DIR / f"note4_flash_{stamp}.bin"

    if out.exists() and not args.force:
        print(f"❌ {out} 已存在。换个名字或加 --force 覆盖。")
        return 2

    # 先取芯片信息，确认连的就是 ESP32-S3
    print("\n>>> 读取芯片信息")
    _esptool([*_esptool_base(port, args.baud, args.no_stub), "flash_id"])
    print("\n>>> 读取 MAC（应记录在案，便于确认备份归属本机设备）")
    _esptool([*_esptool_base(port, args.baud, args.no_stub), "read_mac"])

    chunk = args.chunk_kb * 1024
    print(f"\n>>> 开始全量读取 16 MiB Flash → {out}")
    print(f"    策略：分块 {args.chunk_kb} KiB，每块最多重试 {args.retries} 次")
    print("    期间请勿拔线 / 断电 / 让电脑休眠，也**不要在本机跑重 I/O**"
          "（杀毒扫描、大目录复制都会挤压串口缓冲，导致读块截断）。")

    # 分块读取：任何一次链路抖动只损失一块，不用从头再来
    parts: list[Path] = []
    remaining = FLASH_SIZE
    offset = 0
    ok = True
    while remaining > 0:
        size = min(chunk, remaining)
        part = out.with_suffix(f".part{offset:07X}")
        print(f"\n  [0x{offset:07X}] 读取 {size // 1024} KiB …", flush=True)
        if not _read_chunk(port, args.baud, offset, size, part,
                           no_stub=args.no_stub, retries=args.retries):
            print(f"\n❌ 块 0x{offset:X} 重试 {args.retries} 次仍失败。"
                  "建议：降低 --baud（试 115200）、加 --no-stub，或换一根数据线 / 换个 USB 口。")
            ok = False
            break
        parts.append(part)
        offset += size
        remaining -= size

    if ok:
        print(f"\n>>> 合并 {len(parts)} 个分块 → {out}")
        with out.open("wb") as fh:
            for part in parts:
                fh.write(part.read_bytes())
        for part in parts:           # 合并完成，清掉临时分块
            part.unlink()
        print(f"    合并完成，{out.stat().st_size} 字节")

    if not ok:
        print(f"\n（已读到的 {len(parts)} 个分块保留在 {out.parent}，"
              "文件名形如 *.partXXXXXXX，便于排查或续读）")
        return 1

    res = check_image(out)
    print_report(res)

    manifest = BACKUP_DIR / f"{out.stem}.json"
    manifest.write_text(json.dumps({
        "created_at": _dt.datetime.now().isoformat(timespec="seconds"),
        "port": port,
        "baud": args.baud,
        **res,
    }, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"\n清单已写入 {manifest}")

    if res["ok"]:
        (BACKUP_DIR / f"{out.name}.sha256").write_text(
            f"{res['sha256']}  {out.name}\n", encoding="utf-8")
        print(f"\n★ 下一步必做：把 {out.name} 复制一份到异地（移动硬盘 / 网盘），"
              "然后跑一次空转测试：\n"
              f"    python tools/flash_backup.py restore \"{out}\" --port {port}")
    return 0


def cmd_verify(args: argparse.Namespace) -> int:
    res = check_image(Path(args.file))
    print_report(res)
    return 0 if res.get("ok") else 1


def cmd_restore(args: argparse.Namespace) -> int:
    src = Path(args.file)
    res = check_image(src)
    print_report(res)
    if not res.get("ok"):
        print("\n❌ 镜像未通过自检，拒绝写入。")
        return 2

    port = args.port or _autodetect_port()
    if not port:
        print("❌ 未找到可用串口。")
        return 2

    print(f"\n⚠️  即将把 {src.name} 全量写回设备 {port} 的 0x0 地址。")
    print("    这会覆盖 bootloader + 分区表 + 应用 + NVS（Wi-Fi 配置会被还原成备份时的状态）。")
    if not args.yes:
        reply = input("    确认继续？输入 YES 回车：").strip()
        if reply != "YES":
            print("已取消。")
            return 1

    rc = _esptool([
        "--port", port, "--baud", str(args.baud),
        "--before", "default_reset", "--after", "hard_reset",
        "write_flash", "--flash_size", "16MB", "0x0", str(src),
    ])
    if rc != 0:
        print(f"❌ write_flash 失败（退出码 {rc}）。可先 `erase_flash` 再重试。")
        return rc

    print("\n✅ 写回完成。设备应能正常启动 —— 若正常，说明整条恢复链路已验证可用。")
    return 0


# --------------------------------------------------------------------------- #
def main() -> int:
    ap = argparse.ArgumentParser(
        description="Zectrix Note 4 原厂 Flash 备份 / 校验 / 回写工具",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_b = sub.add_parser("backup", help="全量读取 16 MiB Flash 并自检")
    p_b.add_argument("--port", help="串口，如 COM5；省略则自动探测")
    p_b.add_argument("--baud", type=int, default=460800,
                     help="读取波特率，默认 460800（求稳）；可试 921600")
    p_b.add_argument("--chunk-kb", type=int, default=1024,
                     help="分块大小（KiB），默认 1024。读块截断时调小可减少重试代价")
    p_b.add_argument("--retries", type=int, default=4,
                     help="每块最大重试次数，默认 4")
    p_b.add_argument("--no-stub", action="store_true",
                     help="不加载 RAM stub，直接用 ROM bootloader 读取（慢但最稳）")
    p_b.add_argument("--output", help="输出路径，默认 backup/note4_flash_<时间戳>.bin")
    p_b.add_argument("--force", action="store_true", help="允许覆盖已存在的文件")
    p_b.set_defaults(func=cmd_backup)

    p_v = sub.add_parser("verify", help="对已有备份做自检（不需要连接设备）")
    p_v.add_argument("file", help="备份文件路径")
    p_v.set_defaults(func=cmd_verify)

    p_r = sub.add_parser("restore", help="把备份全量写回设备（空转测试 / 救砖）")
    p_r.add_argument("file", help="备份文件路径")
    p_r.add_argument("--port", help="串口，如 COM5；省略则自动探测")
    p_r.add_argument("--baud", type=int, default=460800, help="写入波特率，默认 460800")
    p_r.add_argument("--yes", action="store_true", help="跳过交互确认（谨慎）")
    p_r.set_defaults(func=cmd_restore)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
