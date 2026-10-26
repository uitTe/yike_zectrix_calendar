#!/usr/bin/env python3
"""AGC 云函数接入链路探针 —— 单独脚本，按 `docs/06-第三方接入华为云函数方法.md` 走两步。

    第一步  POST <TOKEN_URL>   用 client_id + client_secret 换 access_token
    第二步  POST <PROXY_URL>   带 Authorization: Bearer 与 client_id 头调用中转云函数

两步的**完整返回**都会打印：状态码、响应头、响应体。任一步失败立即终止，不重试。

运行（用项目自带环境，`requests` 已装好）：

    tools/.venv/Scripts/python.exe tools/agc_probe.py --check    # 只查参数，不发请求
    tools/.venv/Scripts/python.exe tools/agc_probe.py            # 真打链路

参数不写在本文件里。真实值放 tools/agc_config.local.json（本地文件，不入库，模板见
tools/agc_config.example.json）；也可以加 --from-env 改读环境变量：

    ZECTRIX_AGC_CLIENT_ID / ZECTRIX_AGC_CLIENT_SECRET / ZECTRIX_AGC_API_KEY
    ZECTRIX_AGC_PROXY_URL / ZECTRIX_AGC_GROUP_ID / ZECTRIX_AGC_START_DATE

为什么用 requests 而不是标准库 urllib：`urllib.request` 会把 header 名首字母大写，
`client_id` 会发成 `Client_id`；中转云函数读的是 `event.headers["client_id"]`，
大小写一变就可能判成白名单未命中。requests 原样发送 header 名。
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import sys
import time
from pathlib import Path

import requests

# ===========================================================================
#  以下参数不写在本文件里
#
#  真实值放 tools/agc_config.local.json（本地文件，不入库）；也可以用
#  --from-env 从 ZECTRIX_AGC_* 环境变量读取。两者都没有时参数为空，
#  --check 会逐项报缺。模板见 tools/agc_config.example.json。
# ===========================================================================
CONFIG_FILE = Path(__file__).with_name("agc_config.local.json")
DEFAULT_TOKEN_URL = "https://connect-drcn.dbankcloud.cn/agc/apigw/oauth2/v1/token"


def load_local_config(path: Path) -> dict:
    """读取本地参数文件。文件不存在时返回空字典，缺项由 check_params 报告。"""
    if not path.is_file():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise SystemExit(f"[FAIL] {path.name} 不是合法 JSON：{exc}")
    if not isinstance(data, dict):
        raise SystemExit(f"[FAIL] {path.name} 的顶层必须是对象")
    return data


_LOCAL = load_local_config(CONFIG_FILE)

CLIENT_ID = str(_LOCAL.get("clientId", ""))
CLIENT_SECRET = str(_LOCAL.get("clientSecret", ""))
CUSTOM_API_KEY = str(_LOCAL.get("apiKey", ""))
PRODUCT_ID = str(_LOCAL.get("productId", ""))
TOKEN_URL = str(_LOCAL.get("tokenUrl", DEFAULT_TOKEN_URL))
PROXY_URL_ORI = str(_LOCAL.get("proxyUrl", ""))
GROUP_ID = str(_LOCAL.get("groupId", ""))
START_DATE = str(_LOCAL.get("startDate", ""))  # 格式 YYYY-MM-DD；留空使用北京时间当天日期
# ===========================================================================

ENV_NAMES = {
    "CLIENT_ID": "ZECTRIX_AGC_CLIENT_ID",
    "CLIENT_SECRET": "ZECTRIX_AGC_CLIENT_SECRET",
    "CUSTOM_API_KEY": "ZECTRIX_AGC_API_KEY",
    "PRODUCT_ID": "ZECTRIX_AGC_PRODUCT_ID",
    "PROXY_URL": "ZECTRIX_AGC_PROXY_URL",
    "GROUP_ID": "ZECTRIX_AGC_GROUP_ID",
    "START_DATE": "ZECTRIX_AGC_START_DATE",
}

TIMEOUT = 15          # 秒
BODY_LIMIT = 8000     # 响应体打印上限（字符）


def die(msg: str) -> None:
    print(f"\n[FAIL] {msg}")
    sys.exit(1)


def mask(secret: str) -> str:
    if not secret:
        return "(未填写)"
    return f"{secret[:4]}{'*' * 6}（共 {len(secret)} 字符）"


def dump_response(title: str, resp: requests.Response, cost: float) -> None:
    """打印一次请求的完整返回。"""
    print(f"\n{'-' * 72}")
    print(f" {title} —— 返回")
    print(f"{'-' * 72}")
    print(f"状态码：{resp.status_code} {resp.reason}    （耗时 {cost * 1000:.0f} 毫秒）")
    print(f"最终地址：{resp.url}")

    print("\n响应头：")
    for k, v in resp.headers.items():
        print(f"  {k}: {v}")

    print("\n响应体：")
    text = resp.text
    if len(text) > BODY_LIMIT:
        print(text[:BODY_LIMIT])
        print(f"\n  … 响应体共 {len(text)} 字符，此处只显示前 {BODY_LIMIT} 字符")
    else:
        print(text if text else "(空)")
    print()


def explain(step: str, status: int, body: str) -> None:
    """按文档第 6 节的校验层级给出失败原因。"""
    if step == "token":
        if status in (400, 401):
            print("  原因提示：token 接口拒绝 → 核对 CLIENT_ID / CLIENT_SECRET，"
                  "以及 AGC 控制台里该应用的 OAuth 客户端是否启用。")
        return
    if status == 401 and "invalid api key" in body.lower():
        print("  原因提示：第二层校验失败 → body 的 apiKey 与中转函数环境变量 "
              "CUSTOM_API_KEY 不一致。")
    elif status == 401:
        print("  原因提示：第一层校验失败 → access_token 无效或已过期。")
    elif status == 403:
        print("  原因提示：第三层校验失败 → HTTP 头 client_id 缺失，"
              "或该 clientId 不在 ALLOWED_CLIENT_IDS 白名单里。")
    elif status == 400:
        print("  原因提示：检查 body.data.groupId 和 body.data.startDate 的值及格式。")


def get_token(cfg: dict) -> str:
    payload = {
        "grant_type": "client_credentials",
        "client_id": cfg["CLIENT_ID"],
        "client_secret": cfg["CLIENT_SECRET"],
        "useJwt": 0,
    }
    print(f"{'=' * 72}")
    print(" 第一步 —— 换取 AGC access_token")
    print(f"{'=' * 72}")
    print(f"POST {cfg['TOKEN_URL']}")
    print("Content-Type: application/json")
    print("请求体：" + json.dumps(
        {**payload, "client_secret": mask(cfg["CLIENT_SECRET"])}, ensure_ascii=False))

    t0 = time.monotonic()
    try:
        resp = requests.post(cfg["TOKEN_URL"], json=payload, timeout=TIMEOUT)
    except requests.RequestException as e:
        die(f"token 请求未能发出：{e!r}")
    cost = time.monotonic() - t0

    dump_response("第一步 token", resp, cost)

    if resp.status_code != 200:
        explain("token", resp.status_code, resp.text)
        die(f"第一步失败，HTTP {resp.status_code}")

    try:
        data = resp.json()
    except ValueError as e:
        die(f"第一步返回不是合法 JSON：{e}")

    token = data.get("access_token")
    if not token:
        ret = data.get("ret") if isinstance(data.get("ret"), dict) else {}
        print("[FAIL] 返回里没有 access_token。")
        print(f"       顶层键：{sorted(data)}")
        if ret:
            print(f"       华为业务返回：ret.code = {ret.get('code')}，ret.msg = {ret.get('msg')}")
            msg = str(ret.get("msg", "")).lower()
            if "invalid client id" in msg:
                print("       原因提示：CLIENT_ID 不被识别 —— 核对 AGC 控制台的 Client ID，"
                      "并确认该应用已开通对应的 API 权限。")
            elif "client id or secret error" in msg:
                print("       原因提示：ID/Secret 配对不通过 —— 逐项核对：")
                print("        ① Secret 是否在控制台被重置过（重置后旧值立即失效）")
                print("        ② ID 与 Secret 是否取自同一个应用（勿跨应用混用）")
                print("        ③ 是否为「OAuth 客户端」凭据 —— AGC 里 App ID / API Key / "
                      "OAuth Client ID 是三样不同的东西")
                print("        ④ 项目所在区域是否与 TOKEN_URL 的 -drcn 一致（中国站）")
        die("第一步失败（华为 AGC 用 HTTP 200 包装业务错误，只看状态码会漏判）")

    token_path = Path(__file__).resolve().with_name("agc_access_token.txt")
    timestamp = dt.datetime.now(dt.timezone(dt.timedelta(hours=8))).strftime("%Y-%m-%d %H:%M:%S %z")
    with token_path.open("a", encoding="utf-8") as token_file:
        token_file.write(f"\n[{timestamp}]\n{token}\n")
    print(f"[OK] access_token 已按北京时间追加到：{token_path}")

    expires = data.get("expires_in")
    if expires:
        print(f"[OK] 拿到 access_token（{len(token)} 字符），"
              f"expires_in = {expires} 秒 → 固件须在这个时长内刷新\n")
    else:
        print(f"[OK] 拿到 access_token（{len(token)} 字符），返回里没有 expires_in\n")
    return token


def call_proxy(cfg: dict, token: str) -> requests.Response:
    headers = {
        # 文档 4.3 节的代码读 event.headers["client_id"] 做白名单校验。
        # 文档 5.2 节的请求示例漏写了这个头，缺它会直接 403。
        "clientId": cfg["CLIENT_ID"],
        "Authorization": f"Bearer {token}",
        "productId": cfg["PRODUCT_ID"],
        "Content-Type": "application/json",
    }
    body = {
        "apiKey": cfg["CUSTOM_API_KEY"],
        "data": {
            "groupId": cfg["GROUP_ID"],
            "startDate": cfg["START_DATE"],
        },
    }

    print(f"{'=' * 72}")
    print(" 第二步 —— 调用中转云函数")
    print(f"{'=' * 72}")
    print(f"POST {cfg['PROXY_URL']}")
    for k, v in headers.items():
        print(f"{k}: {mask(v[len('Bearer '):]) if k == 'Authorization' else v}")
    print("请求体：" + json.dumps(
        {**body, "apiKey": mask(cfg["CUSTOM_API_KEY"])}, ensure_ascii=False))

    t0 = time.monotonic()
    try:
        resp = requests.post(cfg["PROXY_URL"], headers=headers, json=body, timeout=TIMEOUT)
    except requests.RequestException as e:
        die(f"中转函数请求未能发出：{e!r}")
    cost = time.monotonic() - t0

    dump_response("第二步 中转云函数", resp, cost)

    if resp.status_code != 200:
        explain("proxy", resp.status_code, resp.text)
        die(f"第二步失败，HTTP {resp.status_code}")

    # 中转接口使用业务码 200 表示查询成功。
    try:
        payload = resp.json()
    except ValueError:
        print("[WARN] 返回不是合法 JSON，无法继续检查业务码")
        return resp
    if isinstance(payload, dict) and payload.get("code") not in (0, 200, None):
        print(f"[FAIL] 第二步业务失败：code = {payload.get('code')}，"
              f"message = {payload.get('message')}")
        die("中转云函数返回业务错误")
    return resp


def check_date_header(resp: requests.Response) -> None:
    """响应头 Date 是 docs/03 6.1 节的 RTC 校准来源。"""
    raw = resp.headers.get("Date", "")
    if not raw:
        print("[WARN] 返回里没有 Date 头 —— 6.1 节的 RTC 校准将退化到 NTP")
        return
    from email.utils import parsedate_to_datetime
    try:
        server = parsedate_to_datetime(raw)
    except Exception as e:  # noqa: BLE001 —— 探针要报原始原因
        print(f"[WARN] Date 头无法解析：{raw!r}（{e}）—— 6.1 节校准会失败")
        return
    drift = abs((dt.datetime.now(dt.timezone.utc) - server).total_seconds())
    print(f"[OK] Date 头可解析：{server.isoformat()}（与本机相差 {drift:.0f} 秒）")
    if drift > 300:
        print(f"[WARN] 与本机差异 {drift / 60:.0f} 分钟 —— 校准时注意时区或本地时钟")


def find_days(payload):
    """从返回体里找日期数组，容忍包在 data / result / schedules 等键里。"""
    if isinstance(payload, list):
        return payload
    if isinstance(payload, dict):
        for k in ("days", "data", "list", "items", "result", "schedules"):
            v = payload.get(k)
            if isinstance(v, list):
                return v
            if isinstance(v, dict):
                for k2 in ("days", "list", "items", "schedules"):
                    if isinstance(v.get(k2), list):
                        return v[k2]
    return None


def check_contract(resp: requests.Response) -> int:
    try:
        payload = resp.json()
    except ValueError as e:
        print(f"[FAIL] 返回不是合法 JSON：{e}")
        return 1

    print(f"{'=' * 72}")
    print(" 返回内容检查")
    print(f"{'=' * 72}")
    print("原始 JSON：")
    print(json.dumps(payload, ensure_ascii=False, indent=2)[:BODY_LIMIT])

    days = find_days(payload)
    if days is None:
        keys = sorted(payload) if isinstance(payload, dict) else type(payload).__name__
        print(f"[FAIL] 返回体里找不到日期数组（顶层键：{keys}）")
        return 1
    print(f"[OK] 找到日期数组，共 {len(days)} 条")

    required = ("date",)
    optional = ("shiftName", "amStart", "amEnd", "pmStart", "pmEnd", "remark")
    bad = 0
    for i, d in enumerate(days, 1):
        if not isinstance(d, dict):
            print(f"[FAIL] 第 {i} 条不是对象：{d!r}")
            bad += 1
            continue
        tag = d.get("date", f"#{i}")
        if not d.get("date"):
            print(f"[FAIL] {tag}: 缺必填字段 date")
            bad += 1
            continue
        if len(str(d["date"])) != 10 or str(d["date"])[4] != "-" or str(d["date"])[7] != "-":
            print(f"[FAIL] {tag}: date 格式应为 YYYY-MM-DD，实际 {d['date']!r}")
            bad += 1
            continue
        for f in optional:
            v = d.get(f)
            if v in (None, ""):
                continue
            if f in ("amStart", "amEnd", "pmStart", "pmEnd"):
                dt.time.fromisoformat(str(v))
        unknown = sorted(set(d) - set(required) - set(optional))
        if unknown:
            print(f"[WARN] {tag}: 多出未用字段 {unknown}（固件忽略，注意协议变动）")

    if bad:
        return 1

    valid = [d for d in days if isinstance(d, dict) and d.get("date")]
    if valid:
        first = dt.date.fromisoformat(valid[0]["date"])
        last = dt.date.fromisoformat(valid[-1]["date"])
        span = (last - first).days + 1
        print(f"[OK] 日期范围 {first} → {last}（{span} 天）")
        if span < 14:
            print(f"[WARN] 窗口只有 {span} 天 < 14 天 —— 断网兜底会提前用尽（6.2 节）")
    return 0


def resolve_config(from_env: bool) -> dict:
    cfg = {
        "CLIENT_ID": CLIENT_ID,
        "CLIENT_SECRET": CLIENT_SECRET,
        "CUSTOM_API_KEY": CUSTOM_API_KEY,
        "PRODUCT_ID": PRODUCT_ID,
        "TOKEN_URL": TOKEN_URL,
        "PROXY_URL": PROXY_URL_ORI,
        "GROUP_ID": GROUP_ID,
        "START_DATE": START_DATE,
    }
    if from_env:
        for key, env_name in ENV_NAMES.items():
            value = os.environ.get(env_name, "")
            if value:
                cfg[key] = value
    if not cfg["START_DATE"]:
        today = dt.datetime.now(dt.timezone(dt.timedelta(hours=8))).date()
        cfg["START_DATE"] = today.isoformat()
        print(f"[提示] START_DATE 未填写，使用北京时间当天日期：{cfg['START_DATE']}")
    return cfg


def check_params(cfg: dict) -> None:
    required = {
        "PROXY_URL": "中转云函数的完整地址（以 /functions/<projectId>/proxy-function-$latest 结尾）",
        "CLIENT_ID": "AGC 控制台的 Client ID",
        "CLIENT_SECRET": "AGC 控制台的 Client Secret",
        "CUSTOM_API_KEY": "与中转云函数环境变量 CUSTOM_API_KEY 一致的值",
        "PRODUCT_ID": "AGC 网关 productId 请求头（路由标识，不是密钥）",
        "GROUP_ID": "中转云函数使用的分组编号",
        "START_DATE": "查询开始日期，格式为 YYYY-MM-DD",
    }
    print("--- 参数检查 ---")
    missing = []
    for key, desc in required.items():
        if cfg.get(key):
            print(f"[OK] {key} 已填写")
        else:
            print(f"[FAIL] {key} 未填写 —— {desc}")
            missing.append(key)
    print(f"[OK] TOKEN_URL = {cfg['TOKEN_URL']}")
    print(f"[OK] GROUP_ID = {cfg['GROUP_ID']}")
    print(f"[OK] START_DATE = {cfg['START_DATE']}")
    if missing:
        die(f"缺少 {len(missing)} 个参数：{', '.join(missing)}。"
            f"填进 {CONFIG_FILE.name}（模板 agc_config.example.json），或改用 --from-env 从环境变量读。")
    dt.date.fromisoformat(cfg["START_DATE"])


def main() -> int:
    ap = argparse.ArgumentParser(
        description="AGC 云函数接入链路探针（见 docs/06-第三方接入华为云函数方法.md）",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--from-env", action="store_true",
                    help="参数改从环境变量读取（ZECTRIX_AGC_*），优先于文件顶部常量")
    ap.add_argument("--check", action="store_true", help="只检查参数是否齐备，不发请求")
    args = ap.parse_args()

    cfg = resolve_config(args.from_env)
    check_params(cfg)
    if args.check:
        print("\n参数齐备。去掉 --check 即可真打链路。")
        return 0

    if not cfg["TOKEN_URL"].startswith("https://") or not cfg["PROXY_URL"].startswith("https://"):
        die("两个地址都必须是 https://（文档第 8 节要求全程 HTTPS）")

    token = get_token(cfg)
    resp = call_proxy(cfg, token)

    check_date_header(resp)
    bad = check_contract(resp)

    print(f"{'=' * 72}")
    if bad == 0:
        print(" 链路连通，返回内容符合数据契约 —— 可以开始翻译成 C（esp_http_client）")
    else:
        print(f" 链路连通，但返回内容有 {bad} 处不符合数据契约 —— 先与第三方对齐字段")
    print(f"{'=' * 72}")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
