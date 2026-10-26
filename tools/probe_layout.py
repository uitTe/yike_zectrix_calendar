"""验证 Pillow 的字符串排版能否用「逐字 + 整数笔位」精确复现。

固件端（C）没有 FreeType，只能按预生成的字形点阵逐个贴图。要让板端 framebuffer
与 PC 侧渲染器逐字节一致，就必须知道 Pillow 到底怎么排每个字的位置。本脚本把
整串绘制的结果与三种「逐字整数笔位」假设的结果逐像素比对，选出能精确复现的那一种。

用法：python tools/probe_layout.py
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from PIL import Image, ImageDraw  # noqa: E402

import calendar_preview as cp  # noqa: E402

CANVAS_W, CANVAS_H = 900, 400
ORIGIN = (40, 160)          # 绘制原点（给中文留足上下空间）

CASES: list[tuple[str, int]] = [
    ("2026年9月23日", 28),
    ("八月十三", 21),
    ("中秋节", 18),
    ("9DP0(验车)", 28),
    ("上午08:00-12:00", 25),
    ("今日班次", 23),
    ("休息", 72),
    ("星期", 44),
    ("秋分", 31),
    ("获取失败", 30),
    ("国庆节", 18),
    ("龙抬头", 18),
    ("凌晨06:00-08:00", 25),
    # 下面这些专门用来暴露小数步进的累积：拉丁数字步进通常不是整像素
    ("0147", 25),
    ("111111111111", 28),
    ("18:00-23:59", 25),
    ("9NP1(绿)", 28),
    ("2026年12月31日", 28),
    ("九月初九", 21),
    ("上午10:30-11:45", 25),
    ("一二三四五六日", 44),
    ("今日班次", 28),
    ("获取失败", 25),
]


def render_whole(s: str, pt: int) -> Image.Image:
    img = Image.new("L", (CANVAS_W, CANVAS_H), 255)
    d = ImageDraw.Draw(img)
    d.text(ORIGIN, s, font=cp.load_font(cp.FONT_FILE, pt), fill=0)
    return img


def render_per_char(s: str, pt: int, mode: str) -> Image.Image:
    """逐字贴图。mode 决定笔位取整方式。"""
    img = Image.new("L", (CANVAS_W, CANVAS_H), 255)
    d = ImageDraw.Draw(img)
    font = cp.load_font(cp.FONT_FILE, pt)
    pen64 = 0                       # 26.6 定点笔位
    pen_float = 0.0
    for ch in s:
        if mode == "round":
            px = ORIGIN[0] + round(pen_float)
        elif mode == "floor":
            px = ORIGIN[0] + int(pen_float)
        elif mode == "fixed64":
            px = ORIGIN[0] + ((pen64 + 32) >> 6)
        else:
            raise ValueError(mode)
        d.text((px, ORIGIN[1]), ch, font=font, fill=0)
        adv64 = round(font.getlength(ch) * 64)
        pen64 += adv64
        pen_float += font.getlength(ch)
    return img


def ink_bbox(img: Image.Image) -> tuple[int, int, int, int]:
    bw = img.point(lambda p: 255 if p <= 127 else 0)
    box = bw.getbbox()
    if box is None:
        raise SystemExit("渲染结果为空，参数有误")
    return box


def check_union(s: str, pt: int) -> str:
    """整串墨迹包围盒 == 各字按笔位摆放后墨迹包围盒的并集？"""
    d = ImageDraw.Draw(Image.new("L", (4, 4), 255))
    font = cp.load_font(cp.FONT_FILE, pt)
    whole = d.textbbox((0, 0), s, font=font)
    pen = 0.0
    xs0, ys0, xs1, ys1 = [], [], [], []
    for ch in s:
        b = d.textbbox((round(pen), 0), ch, font=font)
        xs0.append(b[0]); ys0.append(b[1]); xs1.append(b[2]); ys1.append(b[3])
        pen += font.getlength(ch)
    union = (min(xs0), min(ys0), max(xs1), max(ys1))
    return "一致" if union == whole else f"不一致 整串{whole} 逐字{union}"


def fractional_advances() -> None:
    print("\n--- 非整像素步进的字符（步进的小数部分，单位 1/64 px）---")
    probe = "0123456789:-(）年八月十中秋国庆节休星获取失败龙抬头夏至"
    for pt in (18, 21, 23, 25, 28, 30, 31, 44, 72):
        font = cp.load_font(cp.FONT_FILE, pt)
        odd = []
        for ch in probe:
            v = font.getlength(ch) * 64
            if abs(v - round(v)) > 1e-6:
                odd.append(f"{ch}={v - int(v):+.2f}")
        print(f"  pt{pt:>3}: " + ("、".join(odd) if odd else "全部为整像素步进"))


def diff_count(a: Image.Image, b: Image.Image) -> int:
    ba = a.point(lambda p: 255 if p <= 127 else 0, "1").tobytes()
    bb = b.point(lambda p: 255 if p <= 127 else 0, "1").tobytes()
    return sum(1 for x, y in zip(ba, bb) if x != y)


def main() -> None:
    modes = ["round", "floor", "fixed64"]
    bad = {m: 0 for m in modes}
    union_bad = 0
    print(f"{'字符串':<20}{'pt':>4}  " + "  ".join(f"{m:>9}" for m in modes)
          + "   包围盒并集")
    print("-" * 90)
    for s, pt in CASES:
        whole = render_whole(s, pt)
        wb = ink_bbox(whole)
        cells = []
        for m in modes:
            img = render_per_char(s, pt, m)
            d = diff_count(whole, img)
            if d:
                bad[m] += 1
            cells.append(f"{d:>9}")
        u = check_union(s, pt)
        if u != "一致":
            union_bad += 1
        print(f"{s:<20}{pt:>4}  " + "  ".join(cells) + f"   {u}"
              f"   墨迹宽 {wb[2] - wb[0]}")
    print("-" * 90)
    for m in modes:
        print(f"{m:>9} 不匹配用例数 {bad[m]} / {len(CASES)}")
    print(f"  包围盒并集 不一致用例数 {union_bad} / {len(CASES)}")

    fractional_advances()


if __name__ == "__main__":
    main()
