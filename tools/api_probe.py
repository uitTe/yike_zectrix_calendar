#!/usr/bin/env python3
"""第三方日历接口探针 —— 链路 + 契约 + 时间校准 三重验证。

**为什么要有它**：把「接口到底能不能用」和「固件怎么写」解耦开。
先在 PC 上把接口验通（几秒钟的事），再把它翻译成 C（`esp_http_client`）——
固件里的 HTTP 代码调试成本极高（刷一次 + 抓串口 + 等 00:00），不该用来试错接口。

用法：

    # 1) 离线自检：只验契约解析器本身（不联网）
    tools/.venv/Scripts/python.exe tools/api_probe.py --self-test

    # 2) 真打接口（key 从环境变量读，避免进 shell 历史）
    set ZECTRIX_API_KEY=你的key          # cmd
    export ZECTRIX_API_KEY=你的key       # Git Bash
    tools/.venv/Scripts/python.exe tools/api_probe.py --url https://api.example.com/calendar

    # 3) 覆盖 header 字段名 / 附加查询参数 / 关掉重试
    ... --header-name X-Api-Key --param date=2026-09-23 --retries 1

退出码：0 = 全部通过；1 = 有失败项（便于将来挂进 CI 或 pre-flash 检查）。

⚠️ 待补：`DEFAULT_HEADER` 的字段名（大人后续告知）。在此之前用 --header-name 覆盖。
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from email.utils import parsedate_to_datetime

# --- 待确认项 ---------------------------------------------------------------
DEFAULT_HEADER = "X-Api-Key"      # ⚠️ 业务 key 的 header 字段名 —— 待大人确认
DEFAULT_TIMEOUT = 10              # 秒
DEFAULT_RETRIES = 3               # 与固件侧一致：失败间隔 1 分钟重试 3 次
RETRY_INTERVAL = 60               # 秒
# ---------------------------------------------------------------------------

# 数据契约：第三方只提供这 4 项（详见 docs/03 §2.1）
REQUIRED_FIELDS = ("date",)
OPTIONAL_FIELDS = ("shift", "am", "pm")
TIME_RANGE_RE = re.compile(r"^\d{2}:\d{2}-\d{2}:\d{2}$")
DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")

OK, WARN, BAD = "  OK  ", " WARN ", " FAIL "
_levels = {"OK": 0, "WARN": 0, "FAIL": 0}


def say(tag: str, msg: str) -> None:
    _levels[tag.strip()] += 1
    print(f"[{tag}] {msg}")


def failed() -> int:
    return _levels["FAIL"]


# --- 契约校验 ---------------------------------------------------------------

def _find_days(payload) -> list | None:
    """从响应体里找出「日期数组」。容忍顶层是数组，或包在 days/data/list/items 里。"""
    if isinstance(payload, list):
        return payload
    if isinstance(payload, dict):
        for k in ("days", "data", "list", "items", "result"):
            v = payload.get(k)
            if isinstance(v, list):
                return v
            if isinstance(v, dict):          # 再往下钻一层（如 data.days）
                for k2 in ("days", "list", "items"):
                    if isinstance(v.get(k2), list):
                        return v[k2]
    return None


def check_contract(payload) -> list[dict]:
    """校验并归一化。返回可用的 day 列表（不抛异常，只报告）。"""
    days = _find_days(payload)
    if days is None:
        keys = list(payload.keys()) if isinstance(payload, dict) else type(payload).__name__
        say(BAD, f"响应体里找不到日期数组（顶层键：{keys}）")
        return []

    say(OK, f"找到日期数组，共 {len(days)} 条")
    clean: list[dict] = []

    for i, d in enumerate(days, 1):
        if not isinstance(d, dict):
            say(BAD, f"第 {i} 条不是对象：{d!r}")
            continue
        tag = d.get("date", f"#{i}")

        missing = [f for f in REQUIRED_FIELDS if not d.get(f)]
        if missing:
            say(BAD, f"{tag}: 缺必填字段 {missing}")
            continue
        if not DATE_RE.match(str(d["date"])):
            say(BAD, f"{tag}: date 格式应为 YYYY-MM-DD，实际 {d['date']!r}")
            continue

        # 时段：字段名可能是 am/pm，也可能是 am_time / morning 之类 —— 只报不拦
        for f in OPTIONAL_FIELDS:
            v = d.get(f)
            if v is None or v == "":
                continue
            if f in ("am", "pm") and not TIME_RANGE_RE.match(str(v)):
                say(WARN, f"{tag}: {f}={v!r} 不是 HH:MM-HH:MM（固件将按空值兜底）")

        unknown = set(d) - set(REQUIRED_FIELDS) - set(OPTIONAL_FIELDS)
        if unknown:
            say(WARN, f"{tag}: 多出未用字段 {sorted(unknown)}（固件会忽略，注意协议漂移）")

        clean.append(d)

    if clean:
        d0 = dt.date.fromisoformat(clean[0]["date"])
        d1 = dt.date.fromisoformat(clean[-1]["date"])
        say(OK, f"日期范围 {d0} → {d1}（{(d1 - d0).days + 1} 天）")
        # 拉取窗口策略是「未来 14 天」（docs/03 §6.2），够不够一眼能看出来
        span = (d1 - d0).days + 1
        if span < 14:
            say(WARN, f"窗口仅 {span} 天 < 14 天 —— 断网兜底会提前耗尽（§6.2）")
    return clean


# --- HTTP -------------------------------------------------------------------

def fetch(url: str, header_name: str, key: str, params: list[str]) -> tuple[int, dict, bytes, float]:
    if params:
        sep = "&" if "?" in url else "?"
        url = url + sep + urllib.parse.urlencode(dict(p.split("=", 1) for p in params))

    req = urllib.request.Request(url, headers={
        header_name: key,
        "User-Agent": "zectrix-note4-probe/0.1",
        "Accept": "application/json",
    })
    t0 = time.monotonic()
    try:
        with urllib.request.urlopen(req, timeout=DEFAULT_TIMEOUT) as r:
            return r.status, dict(r.headers), r.read(), time.monotonic() - t0
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers or {}), e.read(), time.monotonic() - t0
    except Exception as e:                     # noqa: BLE001 —— 探针要报原始原因
        return 0, {}, repr(e).encode(), time.monotonic() - t0


def probe(url: str, header_name: str, key: str, params: list[str], retries: int) -> int:
    print(f"→ GET {url}")
    print(f"  header: {header_name}: {'*' * min(len(key), 8)}…（{len(key)} 字符）\n")

    for attempt in range(1, retries + 1):
        status, headers, body, cost = fetch(url, header_name, key, params)
        print(f"--- 第 {attempt}/{retries} 次尝试：{cost * 1000:.0f} ms ---")

        if status == 0:
            say(BAD, f"请求异常：{body.decode('utf-8', 'replace')[:200]}")
        elif status != 200:
            say(BAD, f"HTTP {status}（非 200）")
            snippet = body.decode("utf-8", "replace")[:200]
            if status in (401, 403):
                say(WARN, "401/403 → 大概率是 key 字段名不对或 key 无效，先试 --header-name")
            print(f"        body: {snippet}")
        else:
            say(OK, f"HTTP 200，{len(body)} 字节")
            # 响应头 Date —— §6.1 的 RTC 校准就靠它，必须能解析
            raw_date = headers.get("Date", "")
            if not raw_date:
                say(WARN, "响应里没有 Date 头 —— §6.1 的 RTC 校准将退化到 NTP")
            else:
                try:
                    srv = parsedate_to_datetime(raw_date)
                    drift = abs((dt.datetime.now(dt.timezone.utc) - srv).total_seconds())
                    say(OK, f"Date 头可解析：{srv.isoformat()}（与本机差 {drift:.0f} s）")
                    if drift > 300:
                        say(WARN, f"与本机时间差 {drift / 60:.0f} 分钟 —— 校准时注意时区/本地钟是否准")
                except Exception:              # noqa: BLE001
                    say(BAD, f"Date 头无法解析：{raw_date!r} —— §6.1 校准会失败")

            try:
                payload = json.loads(body)
            except Exception as e:             # noqa: BLE001
                say(BAD, f"响应不是合法 JSON：{e}")
                print(f"        body: {body.decode('utf-8', 'replace')[:300]}")
            else:
                clean = check_contract(payload)
                if clean:
                    print("\n--- 归一化结果（前 5 条）---")
                    for d in clean[:5]:
                        print("   ", json.dumps(d, ensure_ascii=False))
                    if len(clean) > 5:
                        print(f"    … 另有 {len(clean) - 5} 条")

        if failed() == 0:
            break
        if attempt < retries:
            print(f"\n{failed()} 项失败，{RETRY_INTERVAL}s 后重试（模拟固件 §2.4 的退避）…\n")
            time.sleep(RETRY_INTERVAL)
    return failed()


# --- 离线自检 ---------------------------------------------------------------

def self_test() -> int:
    """不联网，只验契约解析器。用正式 mock 数据跑，防解析器本身写错。"""
    here = os.path.dirname(os.path.abspath(__file__))
    mock = os.path.join(here, "mock_calendar_payload.json")
    print(f"离线自检 —— 解析 {os.path.basename(mock)}\n")
    with open(mock, encoding="utf-8") as f:
        payload = json.load(f)
    clean = check_contract(payload)
    print(f"\n解析出 {len(clean)} 天。")

    print("\n--- 用例（每条都该命中它的期望级别）---")
    # 时段格式异常只该 WARN：固件按空值兜底（docs/03 §3.2），不该拦下来
    cases = [
        ("缺 date",      "FAIL", [{"shift": "9DP1"}]),
        ("date 格式错",  "FAIL", [{"date": "2026/09/23"}]),
        ("找不到数组",   "FAIL", {"foo": "bar"}),
        ("时段格式异常", "WARN", [{"date": "2026-09-23", "am": "8 点到 12 点"}]),
        ("顶层直接是数组", "OK",  [{"date": "2026-09-23", "shift": "9DP1"}]),
    ]
    holes = 0
    for name, expect, case in cases:
        before = _levels[expect]
        check_contract(case)
        hit = _levels[expect] > before
        holes += not hit
        print(f"        ↑ 「{name}」期望 {expect} —— {'✓ 命中' if hit else '✗ 漏检（解析器有洞）'}")

    print(f"\n{'✅ 解析器自检通过' if holes == 0 else f'❌ 有 {holes} 个洞'}，"
          f"其余用例不影响退出码（--self-test 恒返回 0）。")
    return 0


# --- main -------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", help="第三方接口地址（https）")
    ap.add_argument("--header-name", default=DEFAULT_HEADER,
                    help=f"业务 key 的 header 字段名（默认 {DEFAULT_HEADER}）")
    ap.add_argument("--key", help="业务 key（不传则读环境变量 ZECTRIX_API_KEY）")
    ap.add_argument("--key-env", default="ZECTRIX_API_KEY", help="读取 key 的环境变量名")
    ap.add_argument("--param", action="append", default=[], metavar="K=V",
                    help="附加查询参数，可重复")
    ap.add_argument("--retries", type=int, default=DEFAULT_RETRIES, help="最大尝试次数")
    ap.add_argument("--self-test", action="store_true", help="只跑离线契约自检，不联网")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    if not args.url:
        ap.error("需要 --url（或用 --self-test 只跑离线自检）")
    if not args.url.startswith("https://"):
        print("⚠️  接口约定是 HTTPS（docs/03 §5 第 8 条），当前不是 https://\n")

    key = args.key or os.environ.get(args.key_env, "")
    if not key:
        print(f"✗ 没有 key：用 --key 传入，或设环境变量 {args.key_env}")
        print(f"  例：set {args.key_env}=你的key")
        return 1

    code = probe(args.url, args.header_name, key, args.param, args.retries)
    print()
    if code == 0:
        print("✅ 全部通过 —— 接口可用，可以开始翻译成 C（esp_http_client）了。")
    else:
        print(f"❌ {code} 项失败 —— 先解决接口侧问题，别带去固件里调。")
    return 1 if code else 0


if __name__ == "__main__":
    sys.exit(main())
