"""生成历法黄金向量（C 头文件），供 tools/almanac_test.c 逐项比对。

**为什么要有这层**：C 版（app/components/cal_almanac/）必须与 Python 参照实现
（tools/lunar_cn.py + tools/solar_terms.py）**逐字节一致**。向量由 Python 生成、
C 侧独立重算并比对 —— 只有两边同时算错才会漏过。

覆盖范围：
    · 闰月表 / 农历年长   1900–2099 全部 200 年
    · 24 节气             1900–2099 全部 200 年 × 24 = 4800 个日期
    · 逐日全信息          14 个代表年（闰年 / 闰月年 / 撞车年 / 表首表尾）
    · 农历串              2026 全年 + 全部闰月初一 + 除夕
    · 除夕 / 多节日撞车日 全量清单

用法：
    tools/.venv/Scripts/python.exe tools/almanac_vectors.py
产出：
    tools/testdata/almanac_vectors.h
"""

from __future__ import annotations

import datetime as dt
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import calendar_A_final as caf  # noqa: E402  badges() = 「清明让位」去重的权威实现
import lunar_cn  # noqa: E402
import solar_terms  # noqa: E402
from lunardate import LunarDate  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "tools" / "testdata" / "almanac_vectors.h"

FIRST_YEAR, LAST_YEAR = 1900, 2099
TERM_ORDER = [n for n, _, _ in solar_terms.TERMS]

CAL_FEST_MAX = 3  # 必须与 app/components/cal_almanac/include/cal_almanac.h 一致

# 逐日全信息的代表年：闰年 / 闰月年 / 节日撞车年 / 表首表尾
DAY_YEARS = [1900, 1901, 2023, 2024, 2025, 2026, 2027, 2030, 2031,
             2033, 2037, 2060, 2088, 2099]


def day_range(y: int):
    """某年逐日；1900 从年表起点 1900-01-31 起。"""
    d = dt.date(1900, 1, 31) if y == FIRST_YEAR else dt.date(y, 1, 1)
    end = dt.date(y, 12, 31)
    while d <= end:
        yield d
        d += dt.timedelta(days=1)


def main() -> int:
    print("生成黄金向量…")

    # ---------------- 闰月表 / 年长 ----------------
    leap_month: list[int] = []
    year_days: list[int] = []
    for y in range(FIRST_YEAR, LAST_YEAR + 1):
        leap_month.append(LunarDate.leap_month_for_year(y) or 0)
        if y < LAST_YEAR:
            a = LunarDate(y, 1, 1, False).to_solar_date()
            b = LunarDate(y + 1, 1, 1, False).to_solar_date()
            year_days.append((b - a).days)
        else:
            year_days.append(-1)  # 2099 无下一年，C 侧跳过
    print(f"  闰月表 / 年长：{len(leap_month)} 年")

    # ---------------- 24 节气 ----------------
    print("  节气：1900–2099 × 24（天文法二分，会慢一点）…")
    term_md: list[tuple[int, int]] = []
    for y in range(FIRST_YEAR, LAST_YEAR + 1):
        ts = solar_terms.terms_of_year(y)
        for name in TERM_ORDER:
            d = ts[name]
            term_md.append((d.month, d.day))
    assert len(term_md) == 200 * 24, len(term_md)
    print(f"  节气：{len(term_md)} 个日期")

    # ---------------- 节日名表 ----------------
    fest_names: list[str] = [""]  # 下标 0 = 无
    fest_index: dict[str, int] = {"": 0}

    def fid(name: str) -> int:
        if name not in fest_index:
            fest_index[name] = len(fest_names)
            fest_names.append(name)
        return fest_index[name]

    # ---------------- 逐日全信息 ----------------
    # raw = cal_festivals() / cal_solar_term_index() 的返回值（未去重）
    # eff = caf.badges() 的结果 = cal_day_info_get() 实际给出的值（含「清明让位」）
    day_rows: list[tuple] = []
    for y in DAY_YEARS:
        for d in day_range(y):
            lu = lunar_cn.to_lunar(d)
            term = solar_terms.term_on(d)
            ti = -1 if term is None else TERM_ORDER.index(term)
            ids = [fid(f) for f in lunar_cn.festivals(d)]
            eff_term, eff_fests = caf.badges(d)
            eff_ti = -1 if eff_term is None else TERM_ORDER.index(eff_term)
            eff_ids = [fid(f) for f in eff_fests]
            day_rows.append((d.year, d.month, d.day,
                             lu.month, lu.day, 1 if lu.isLeapMonth else 0,
                             d.weekday(), ti, ids, eff_ti, eff_ids))
    print(f"  逐日全信息：{len(day_rows)} 天 / {len(DAY_YEARS)} 年")

    # ---------------- 除夕 / 撞车日 ----------------
    eve_rows: list[tuple[int, int, int]] = []
    collide_rows: list[tuple[int, int, int, list[int]]] = []
    for y in range(FIRST_YEAR, LAST_YEAR + 1):
        for d in day_range(y):
            if lunar_cn.is_lunar_new_year_eve(d):
                eve_rows.append((d.year, d.month, d.day))
            f = lunar_cn.festivals(d)
            if len(f) >= 2:
                collide_rows.append((d.year, d.month, d.day, [fid(x) for x in f]))
    print(f"  除夕：{len(eve_rows)} 天 ｜ 多节日撞车：{len(collide_rows)} 天")

    # ---------------- 农历串 ----------------
    lunar_text_rows: list[tuple[int, int, int, str]] = []
    for d in day_range(2026):
        lunar_text_rows.append((d.year, d.month, d.day, lunar_cn.lunar_text(d)))
    for y in range(FIRST_YEAR, LAST_YEAR + 1):
        lm = LunarDate.leap_month_for_year(y)
        if lm:
            d = LunarDate(y, lm, 1, True).to_solar_date()
            lunar_text_rows.append((d.year, d.month, d.day, lunar_cn.lunar_text(d)))
    for y in range(FIRST_YEAR, LAST_YEAR + 1):  # 除夕（含闰月年的边界）
        d = LunarDate(y, 1, 1, False).to_solar_date() - dt.timedelta(days=1)
        if d < dt.date(1900, 1, 31):
            continue  # 1900 的除夕在年表起点之前，属预期越界
        lunar_text_rows.append((d.year, d.month, d.day, lunar_cn.lunar_text(d)))
    print(f"  农历串：{len(lunar_text_rows)} 条")

    # ---------------- 输出 ----------------
    L: list[str] = []
    L.append("/* 自动生成，请勿手改 —— 由 tools/almanac_vectors.py 产出。")
    L.append(" * 内容 = Python 参照实现（lunar_cn.py + solar_terms.py）算出的期望值，")
    L.append(" * 供 tools/almanac_test.c 驱动的 C 版逐项比对。")
    L.append(" */")
    L.append("#ifndef ALMANAC_VECTORS_H")
    L.append("#define ALMANAC_VECTORS_H")
    L.append("")
    L.append('#include <stdint.h>')
    L.append('#include "cal_almanac.h"')
    L.append("")
    L.append(f"#define VEC_FIRST_YEAR {FIRST_YEAR}")
    L.append(f"#define VEC_LAST_YEAR  {LAST_YEAR}")
    L.append(f"#define VEC_YEARS      {LAST_YEAR - FIRST_YEAR + 1}")
    L.append(f"#define VEC_TERM_NUM   {len(TERM_ORDER)}")
    L.append(f"#define VEC_DAY_COUNT  {len(day_rows)}")
    L.append(f"#define VEC_EVE_COUNT  {len(eve_rows)}")
    L.append(f"#define VEC_COLL_COUNT {len(collide_rows)}")
    L.append(f"#define VEC_TEXT_COUNT {len(lunar_text_rows)}")
    L.append("")
    L.append("static const char *const VEC_FEST_NAMES[] = {")
    for n in fest_names:
        L.append(f'    "{n}",')
    L.append("};")
    L.append(f"#define VEC_FEST_NAME_N {len(fest_names)}")
    L.append("")
    L.append("static const char *const VEC_TERM_NAMES[24] = {")
    L.append("    " + ", ".join(f'"{n}"' for n in TERM_ORDER) + ",")
    L.append("};")
    L.append("")
    L.append("/* 每年闰月月份（0 = 无闰月） */")
    L.append("static const uint8_t VEC_LEAP[VEC_YEARS] = {")
    for i in range(0, len(leap_month), 25):
        L.append("    " + ", ".join(str(v) for v in leap_month[i:i + 25]) + ",")
    L.append("};")
    L.append("")
    L.append("/* 每年农历天数（2099 无下一年 → -1，测试跳过） */")
    L.append("static const int16_t VEC_YEARDAYS[VEC_YEARS] = {")
    for i in range(0, len(year_days), 20):
        L.append("    " + ", ".join(str(v) for v in year_days[i:i + 20]) + ",")
    L.append("};")
    L.append("")
    L.append("/* 24 节气日期，每年一组，顺序同 VEC_TERM_NAMES */")
    L.append("static const uint8_t VEC_TERM_MD[VEC_YEARS * 24][2] = {")
    for i in range(0, len(term_md), 8):
        L.append("    " + " ".join("{%d,%d}," % (m, d) for m, d in term_md[i:i + 8]))
    L.append("};")
    L.append("")
    L.append("typedef struct {")
    L.append("    int16_t y, m, d;")
    L.append("    uint8_t lmonth, lday, lleap;")
    L.append("    uint8_t weekday;")
    L.append("    int8_t  term;      /* 原始节气索引，-1 = 无（= cal_solar_term_index） */")
    L.append("    uint8_t fest_n;    /* = cal_festivals 的返回个数 */")
    L.append("    uint8_t fest_id[CAL_FEST_MAX];")
    L.append("    int8_t  term_eff;  /* 去重后实际显示的节气，-1 = 让位（= cal_day_info.term） */")
    L.append("    uint8_t fest_eff_n;")
    L.append("    uint8_t fest_eff_id[CAL_FEST_MAX];")
    L.append("} vec_day;")
    L.append("")
    L.append("static const vec_day VEC_DAYS[VEC_DAY_COUNT] = {")
    for (y, m, d, lm, ld, lp, wd, ti, ids, eti, eids) in day_rows:
        pad = list(ids) + [0] * (CAL_FEST_MAX - len(ids))
        epad = list(eids) + [0] * (CAL_FEST_MAX - len(eids))
        L.append("    {%d,%d,%d, %d,%d,%d, %d, %d, %d, {%s}, %d, %d, {%s}},"
                 % (y, m, d, lm, ld, lp, wd, ti, len(ids),
                    ",".join(str(x) for x in pad),
                    eti, len(eids), ",".join(str(x) for x in epad)))
    L.append("};")
    L.append("")
    L.append("typedef struct { int16_t y, m, d; } vec_ymd;")
    L.append("")
    L.append("static const vec_ymd VEC_EVE[VEC_EVE_COUNT] = {")
    for i in range(0, len(eve_rows), 8):
        L.append("    " + " ".join("{%d,%d,%d}," % r for r in eve_rows[i:i + 8]))
    L.append("};")
    L.append("")
    L.append("typedef struct { int16_t y, m, d; uint8_t n; uint8_t id[CAL_FEST_MAX]; } vec_coll;")
    L.append("")
    L.append("static const vec_coll VEC_COLL[VEC_COLL_COUNT] = {")
    for (y, m, d, ids) in collide_rows:
        pad = list(ids) + [0] * (CAL_FEST_MAX - len(ids))
        L.append("    {%d,%d,%d, %d, {%s}},"
                 % (y, m, d, len(ids), ",".join(str(x) for x in pad)))
    L.append("};")
    L.append("")
    L.append("typedef struct { int16_t y, m, d; const char *text; } vec_text;")
    L.append("")
    L.append("static const vec_text VEC_TEXT[VEC_TEXT_COUNT] = {")
    for (y, m, d, t) in lunar_text_rows:
        L.append(f'    {{{y},{m},{d}, "{t}"}},')
    L.append("};")
    L.append("")
    L.append("#endif /* ALMANAC_VECTORS_H */")
    L.append("")

    text = "\n".join(L)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"  ✅ {OUT.relative_to(ROOT)}（{len(text):,} B / {len(text.splitlines()):,} 行）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
