"""探针：① 顶栏右侧的可用几何 ② 历史上真的会出现的「同日多节日」组合。

目的 —— 实现「同日多节日并列展示」前，先把两件事用数据定下来，而不是拍脑袋：
  ① 顶栏反白签从左到右究竟有多少像素可用（标题右界 → RIGHT_X）；
  ② 真有哪几天会撞车、会撞几个、名字多长 —— 决定并排还是需要兜底。
"""

from __future__ import annotations

import datetime as dt
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from calendar_preview import Canvas, F_CN  # noqa: E402
from lunardate import LunarDate  # noqa: E402
import lunar_cn  # noqa: E402

RIGHT_X = 384

c = Canvas()

print("=== ① 顶栏几何实测 ===")
title = "2026年9月23日"
for pt in (27,):
    tw = c.text_width_pt(title, F_CN, pt)
    print(f"标题 {title!r} pt{pt} → 宽 {tw}px  (x ∈ [16, {16 + tw}])")

print()
print("农历行（对齐到 RIGHT_X=384，右对齐）：")
for s in ("八月十三", "闰六月初一", "腊月三十"):
    for pt in (21, 18, 16):
        print(f"  {s!r:8s} pt{pt} → {c.text_width_pt(s, F_CN, pt):4d}px")

print()
print("反白签（pad_x 各值时）：")
for s in ("中秋节", "国庆节", "春节", "元旦", "重阳节"):
    row = [f"{s!r:6s} pt18"]
    for pad in (10, 8, 6):
        w = c.text_width_pt(s, F_CN, 18) + 2 * pad
        row.append(f"pad{pad}={w:3d}px")
    print("  " + "  ".join(row))

print()
print("两签并排总宽（各 pad 与 gap 组合，目标 ≤ 标题右界→384）：")
one = c.text_width_pt("中秋节", F_CN, 18)
for pad in (10, 8, 6):
    for gap in (14, 12, 10, 8):
        total = 2 * (one + 2 * pad) + gap
        print(f"  pad{pad} gap{gap:2d} → {total:3d}px")

print()
print("=== ② 历史上真实出现的同日多节日（1900–2099）===")
hits: dict[int, int] = {}
examples: list[tuple[dt.date, list[str]]] = []

for year in range(1900, 2100):
    d = dt.date(year, 1, 1)
    while d.year == year:
        found: list[str] = []
        if lunar_cn.is_lunar_new_year_eve(d):
            found.append("除夕")
        lu = LunarDate.from_solar_date(d.year, d.month, d.day)
        if not lu.isLeapMonth:
            n = lunar_cn.FEST_LUNAR.get((lu.month, lu.day))
            if n:
                found.append(n)
        n = lunar_cn.FEST_SOLAR.get((d.month, d.day))
        if n:
            found.append(n)
        if d == lunar_cn._nth_weekday(d.year, 5, 6, 2):
            found.append("母亲节")
        if d == lunar_cn._nth_weekday(d.year, 6, 6, 3):
            found.append("父亲节")
        if d == lunar_cn._nth_weekday(d.year, 11, 3, 4):
            found.append("感恩节")
        if len(found) > 1:
            hits[len(found)] = hits.get(len(found), 0) + 1
            examples.append((d, found))
        d += dt.timedelta(days=1)

print(f"共 {len(examples)} 天撞车，按节日个数分布：{hits}")
print()
print("前 24 例：")
for d, fs in examples[:24]:
    wd = "一二三四五六日"[d.weekday()]
    print(f"  {d} 星期{wd}  {' + '.join(fs)}")

print()
print("撞车最多的几种组合（Top 12）：")
from collections import Counter  # noqa: E402

combo = Counter(tuple(sorted(fs)) for _, fs in examples)
for fs, n in combo.most_common(12):
    print(f"  {n:3d}×  {' + '.join(fs)}")

print()
print("最长组合（按名字总字数）：")
for d, fs in sorted(examples, key=lambda x: -sum(len(f) for f in x[1]))[:8]:
    print(f"  {d}  共 {sum(len(f) for f in fs)} 字  {' + '.join(fs)}")
