#!/usr/bin/env python3
"""开源前的敏感信息扫描 —— 按形态匹配，不依赖具体凭据值。

为什么按形态而不按值：凭据一旦轮换，按值匹配的规则全部失效，而形态（64 位
十六进制、19 位客户端号、grp_ 前缀、URL 里的 projectId）不会变。

用法：

    tools/.venv/Scripts/python.exe tools/secret_scan.py            # 扫描，P0 命中退出码 1
    tools/.venv/Scripts/python.exe tools/secret_scan.py --strict   # P1 也算失败
    tools/.venv/Scripts/python.exe tools/secret_scan.py --list     # 只列文件，不读内容

白名单：占位符（`<client-id>` 一类）、示例值（MyWiFi / example）、`*.example.*`
文件、`*.sha256` 校验文件。命中结果只打印前 6 个字符，避免扫描日志本身泄露。
"""

from __future__ import annotations

import argparse
import fnmatch
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# 不扫的目录：构建产物、虚拟环境、版本库内部、上游代码
# firmware/ 是官方 Demo 的 fork，凭据问题不在它身上，且它有自己的 .gitignore
SKIP_DIRS = {
    ".git", "build", ".venv", "__pycache__", "managed_components",
    "node_modules", ".workbuddy", "firmware",
}
# 不扫的文件：体积过大、已知二进制、依赖锁（内含组件哈希，不是密钥）
SKIP_SUFFIX = {".bin", ".img", ".a", ".o", ".obj", ".pyc", ".png", ".jpg", ".pdf", ".zip", ".lock"}
SKIP_NAME = {"dependencies.lock"}
# 单文件上限，超过就跳过（日志可能很大）
MAX_BYTES = 4 * 1024 * 1024

# 行内出现这些词就不报（哈希字段、占位说明）
LINE_SKIP = re.compile(r"sha256|sha-256|校验|placeholder|占位|<client-|<api-|<group-|<project-", re.I)
# 命中值属于这些示例值时跳过
VALUE_SKIP = {
    "mywifi", "mywifi2.4g", "example", "example-id", "example-product-id", "your-ssid",
    "test", "tests", "pass", "password", "ssid", "value", "zectrix-cal-xxxx",
}
# 值里含中文 ⇒ 是说明文字，不是密钥
CJK = re.compile(r"[\u4e00-\u9fff]")
# 固定地址与文档示例地址，公开无害
ADDR_SKIP = {"192.168.4.1", "192.168.4.0", "192.168.1.7"}
# 文档里用来展示格式的示例标识（6 段 MAC 与 7 段 NFC UID 两种形态）
MAC_SKIP = {"aa:bb:cc:dd:ee:ff", "aa:bb:cc:dd:ee:ff:00"}

RULES = [
    # 级别, 名称, 正则, 取值分组（0 表示整段，1 表示只检查第一组）
    ("P0", "私钥块", re.compile(r"-----BEGIN (?:[A-Z ]+ )?PRIVATE KEY-----"), 0),
    ("P0", "Bearer JWT", re.compile(r"Bearer\s+eyJ[A-Za-z0-9_\-.]{10,}"), 0),
    ("P0", "CLIENT_SECRET 常量", re.compile(r"CLIENT_SECRET\s*=\s*[\"']([^\"'<]{16,})[\"']"), 1),
    ("P0", "CUSTOM_API_KEY 常量", re.compile(r"CUSTOM_API_KEY\s*=\s*[\"']([^\"'<]{8,})[\"']"), 1),
    ("P0", "client_secret 字段", re.compile(r"[\"']client_secret[\"']\s*:\s*[\"']([^\"'<]{8,})[\"']"), 1),
    ("P0", "apiKey 字段", re.compile(r"[\"']apiKey[\"']\s*:\s*[\"']([^\"'<]{6,})[\"']"), 1),
    ("P0", "64 位十六进制密钥", re.compile(r"\b[0-9a-fA-F]{64}\b"), 0),
    ("P1", "19 位 Client ID", re.compile(r"\b1\d{18}\b"), 0),
    ("P1", "groupId", re.compile(r"\bgrp_\d{6,}"), 0),
    ("P1", "云函数 projectId", re.compile(r"/functions/([a-z0-9]{5,})/"), 1),
    ("P1", "WiFi 名（等号）", re.compile(r"(?i)\bssid\s*=\s*([A-Za-z0-9._\-]{5,})"), 1),
    ("P1", "WiFi 名（JSON）", re.compile(r"(?i)[\"']ssid[\"']\s*:\s*[\"']([^\"']{5,})[\"']"), 1),
    ("P1", "MAC 地址", re.compile(r"\b(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}\b"), 0),
    ("P1", "内网地址", re.compile(r"\b(?:192\.168|10\.\d{1,3})\.\d{1,3}\.\d{1,3}\b"), 0),
]


def load_ignore(root: Path) -> list[str]:
    """从根 .gitignore 读排除模式，让扫描范围与提交范围一致。"""
    gi = root / ".gitignore"
    if not gi.is_file():
        return []
    patterns = []
    for line in gi.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or line.startswith("!"):
            continue
        patterns.append(line)
    return patterns


def is_ignored(rel: str, patterns: list[str]) -> bool:
    parts = rel.split("/")
    for pat in patterns:
        p = pat.rstrip("/")
        if not p:
            continue
        if "/" in p:
            if fnmatch.fnmatch(rel, p) or rel.startswith(p + "/"):
                return True
        elif any(fnmatch.fnmatch(part, p) for part in parts):
            return True
    return False


def iter_files(root: Path, patterns: list[str]):
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        if any(part in SKIP_DIRS for part in path.parts):
            continue
        if path.suffix.lower() in SKIP_SUFFIX or path.name.endswith(".sha256"):
            continue
        if path.name in SKIP_NAME or ".example." in path.name:
            continue
        rel = path.relative_to(root).as_posix()
        if patterns and is_ignored(rel, patterns):
            continue
        try:
            if path.stat().st_size > MAX_BYTES:
                continue
        except OSError:
            continue
        yield path


def read_text(path: Path) -> str | None:
    try:
        raw = path.read_bytes()
    except OSError:
        return None
    if b"\x00" in raw[:4096]:
        return None
    return raw.decode("utf-8", errors="ignore")


def mask(text: str) -> str:
    text = text.strip()
    return text[:6] + "…" if len(text) > 6 else text


def scan(root: Path, patterns: list[str]) -> list[tuple[str, str, int, str, str]]:
    hits: list[tuple[str, str, int, str, str]] = []
    for path in iter_files(root, patterns):
        text = read_text(path)
        if text is None:
            continue
        rel = path.relative_to(root).as_posix()
        for lineno, line in enumerate(text.splitlines(), start=1):
            if LINE_SKIP.search(line):
                continue
            for level, name, pattern, group in RULES:
                m = pattern.search(line)
                if not m:
                    continue
                value = m.group(group) if group else m.group(0)
                low = value.strip().lower()
                if low in VALUE_SKIP or low.startswith(("your-", "cjson")):
                    continue
                if low in ADDR_SKIP or low in MAC_SKIP or CJK.search(value):
                    continue
                hits.append((level, rel, lineno, name, mask(value)))
    return hits


def main() -> int:
    ap = argparse.ArgumentParser(description="开源前敏感信息扫描")
    ap.add_argument("--strict", action="store_true", help="P1 命中也算失败")
    ap.add_argument("--list", action="store_true", help="只列出将被扫描的文件数量")
    args = ap.parse_args()

    if args.list:
        files = list(iter_files(ROOT, load_ignore(ROOT)))
        print(f"将扫描 {len(files)} 个文件，根目录 {ROOT}")
        return 0

    patterns = load_ignore(ROOT)
    hits = scan(ROOT, patterns)
    p0 = [h for h in hits if h[0] == "P0"]
    p1 = [h for h in hits if h[0] == "P1"]

    for level, rel, lineno, name, value in hits:
        print(f"[{level}] {rel}:{lineno}  {name}  {value}")

    print(f"\nP0 {len(p0)} 处，P1 {len(p1)} 处。")
    if p0:
        print("P0 必须清零后才能开源：凭据要外置到本地配置，不要写进仓库文件。")
        return 1
    if p1 and args.strict:
        print("P1 命中：属于内部信息或隐私，建议脱敏后再提交。")
        return 1
    print("没有 P0 命中。" + ("（P1 仅提示）" if p1 else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
