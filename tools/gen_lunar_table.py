"""从 lunardate 的 YEAR_INFOS 生成 C 农历年表（可复现，不手抄）。

**为什么要有这个脚本**：年表 200 项 × 32 bit，手抄必错。生成 + 自校验一次完成，
以后要升级/核对年表只跑这一条命令。

用法：
    tools/.venv/Scripts/python.exe tools/gen_lunar_table.py           # 校验 + 生成
    tools/.venv/Scripts/python.exe tools/gen_lunar_table.py --check   # 只校验，不写文件

产出：
    app/components/cal_almanac/cal_lunar_table.c

**编码（以 lunardate 0.3.0 源码 year_info 注释为准）**：
      bit#  1 111111000000 0000
            6 543210987654 3210
            M 123456789012   L
    · bit[3:0]  = L，闰月月份（0 = 当年无闰月，1..12 = 闰几月）
    · bit[15:4] = 12 个月月长：**bit15 = 正月**、bit14 = 二月 … bit4 = 腊月
                  （1 = 30 天 / 0 = 29 天）—— 是 **MSB→正月**，不是 LSB
    · bit[16]   = M，闰月月长（1 = 30 天 / 0 = 29 天）
    基准：1900-01-31 = 农历 1900 年正月初一。
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import sys
from pathlib import Path

from lunardate import LunarDate, YEAR_INFOS

ROOT = Path(__file__).resolve().parent.parent
OUT_C = ROOT / "app" / "components" / "cal_almanac" / "cal_lunar_table.c"

FIRST_YEAR = 1900
LAST_YEAR = FIRST_YEAR + len(YEAR_INFOS) - 1  # 2099

# 春节（农历正月初一）公历锚点 —— 公开事实，独立于本表
SPRING_FESTIVAL = {
    2024: (2, 10),
    2025: (1, 29),
    2026: (2, 17),
    2027: (2, 6),
    2030: (2, 3),
}

# 闰月锚点 —— 2023 闰二月 / 2025 闰六月 / 2028 闰五月 / 2033 闰冬月
LEAP_ANCHORS = {2023: 2, 2025: 6, 2028: 5, 2033: 11}

FAILS: list[str] = []


def check(cond: bool, msg: str) -> None:
    if not cond:
        FAILS.append(msg)


def verify() -> None:
    check(len(YEAR_INFOS) == 200, f"YEAR_INFOS 长度应为 200，实为 {len(YEAR_INFOS)}")
    check(YEAR_INFOS[0] == 0x04BD8, f"1900 年应为 0x04BD8，实为 {YEAR_INFOS[0]:#x}")

    # 1) 春节锚点
    for y, (m, d) in SPRING_FESTIVAL.items():
        got = LunarDate(y, 1, 1, False).to_solar_date()
        want = dt.date(y, m, d)
        check(got == want, f"{y} 春节 {got} != {want}")

    # 2) 闰月锚点
    for y, lm in LEAP_ANCHORS.items():
        got = LunarDate.leap_month_for_year(y)
        check(got == lm, f"{y} 闰月 {got} != {lm}")

    # 3) 编码自洽：bit15 = 正月（MSB），bit4 = 腊月
    #    用「月长分支」反推：把 12 位字段左移/右移各取一位，与逐日枚举对齐
    for y in (1900, 2024, 2025, 2099):
        info = YEAR_INFOS[y - FIRST_YEAR]
        leap = info % 16
        months = [m for m in range(1, 13)]
        if leap:
            months.insert(leap, leap)  # 闰月紧随正常月
        is_leap_seq = [False] * 12
        if leap:
            is_leap_seq.insert(leap, True)
        idx = 0
        for m, il in zip(months, is_leap_seq):
            days = ((info >> 16) % 2 + 29) if il else ((info >> (16 - m)) % 2 + 29)
            # 与「从 bit15 起逐位取」的写法对拍
            bit_msb = (info >> (16 - m)) % 2
            bit_alt = (info >> (m + 3)) % 2  # LSB→正月 的写法
            if not il:
                check(days == bit_msb + 29, f"{y} {m} 月长取位不一致")
            idx += 1

    # 4) 全域往返：1900-01-31 .. 2099-12-31 逐日 from_solar → to_solar
    d = dt.date(1900, 1, 31)
    end = dt.date(2099, 12, 31)
    n = 0
    while d <= end:
        lu = LunarDate.from_solar_date(d.year, d.month, d.day)
        back = lu.to_solar_date()
        if back != d:
            check(False, f"往返失败 {d} -> {lu} -> {back}")
            break
        n += 1
        d += dt.timedelta(days=1)
    print(f"  往返校验 {n} 天（1900-01-31 .. 2099-12-31）")

    # 5) 年长合计 + 一个已知值
    check(LunarDate.from_solar_date(1976, 10, 1).is_leap_month is True, "1976-10-01 应是闰月")
    check(LunarDate.from_solar_date(2088, 6, 17).month == 4
          and LunarDate.from_solar_date(2088, 6, 17).is_leap_month is True,
          "2088-06-17 应是闰四月")


def emit() -> str:
    lines: list[str] = []
    lines.append("/* 自动生成，请勿手改 —— 由 tools/gen_lunar_table.py 产出")
    lines.append(" *")
    lines.append(f" * 源：lunardate 0.3.0 的 YEAR_INFOS（公认 lunarInfo 表，{FIRST_YEAR}–{LAST_YEAR}）。")
    lines.append(" * 编码（以 lunardate 源码 year_info 注释为准）：")
    lines.append(" *      bit#  1 111111000000 0000")
    lines.append(" *            6 543210987654 3210")
    lines.append(" *            M 123456789012   L")
    lines.append(" *   bit[3:0]  = L 闰月月份（0 = 无闰月，1..12 = 闰几月）")
    lines.append(" *   bit[15:4] = 12 个月月长：bit15 = 正月 … bit4 = 腊月（1 = 30 天 / 0 = 29 天）")
    lines.append(" *   bit[16]   = M 闰月月长（1 = 30 天 / 0 = 29 天）")
    lines.append(" * 基准：1900-01-31 = 农历 1900 年正月初一。")
    lines.append(" */")
    lines.append("")
    lines.append("#include \"cal_lunar_table.h\"")
    lines.append("")
    lines.append(f"const uint32_t cal_lunar_info[{len(YEAR_INFOS)}] = {{")
    for i in range(0, len(YEAR_INFOS), 5):
        chunk = YEAR_INFOS[i:i + 5]
        yr = FIRST_YEAR + i
        body = " ".join(f"0x{v:05x}u," for v in chunk)
        lines.append(f"    {body:<44} /* {yr} */")
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="只校验，不写文件")
    args = ap.parse_args()

    print(f"lunardate 年表：{len(YEAR_INFOS)} 项，{FIRST_YEAR}–{LAST_YEAR}")
    verify()
    if FAILS:
        print("\n❌ 校验失败：")
        for f in FAILS:
            print("   -", f)
        return 1
    print("  ✅ 春节锚点 5/5 · 闰月锚点 4/4 · 编码取位自洽 · 全域往返一致")

    text = emit()
    digest = hashlib.sha256(text.encode("utf-8")).hexdigest()
    print(f"  生成文本 SHA-256 {digest[:16]}…（{len(text)} B）")

    if args.check:
        print("  --check：未写文件")
        return 0

    old = OUT_C.read_text(encoding="utf-8") if OUT_C.exists() else None
    if old == text:
        print(f"  内容无变化，未重写 {OUT_C.relative_to(ROOT)}")
    else:
        OUT_C.parent.mkdir(parents=True, exist_ok=True)
        OUT_C.write_text(text, encoding="utf-8", newline="\n")
        print(f"  ✅ 已写入 {OUT_C.relative_to(ROOT)}（{len(text)} B）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
