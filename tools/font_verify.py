"""字模数据一致性验证（PC 侧）。

固件端没有 FreeType，只能按导出的字模数据逐字贴图。本脚本用与固件完全相同的
「整数笔位 + 双盒子」模型在 PC 上重放一遍绘制，并与 PC 侧渲染器
（calendar_preview.Canvas）的实际绘制逐像素比对 —— 这是板端 framebuffer 能否
与渲染器一致的前提。

⚠️ 改字库后**必须重跑本脚本**。

两类断言，缺一不可
------------------
1. **对齐断言（pt 级）** —— 渲染器实际选出的每个 pt 都真在字库内。
2. **字符断言（字级）** —— 每个用例里的**每一个字符**都真在字库的那个字号组内。

少了第 2 条会**静默漏掉缺字**：`font_bitmap.glyph_metrics()` 用 Pillow 能渲染任意
字符，于是「重建模型」和「参照渲染」两边都画得出，比对必然零差异 —— 但**固件端
查不到那个字形**（`cal_font_lookup` 返回 NULL → `RequireGlyph` 直接 `abort()`）。
2026-10-14 就是这样漏掉了班次名里的「值班」二字，由大人肉眼发现。

用法：python tools/font_verify.py
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from PIL import Image  # noqa: E402

import calendar_preview as cp  # noqa: E402
import font_bitmap  # noqa: E402
import pt_census  # noqa: E402

CANVAS = (cp.W, cp.H)

# (文本, target_h, 锚点中心 x, 对齐)
# 按**区域**枚举，覆盖普查出的全部 pt 段位：
#   顶栏年月日 pt28 ｜ 农历无节日 pt17–21 ｜ 农历有节日 pt15–18 ｜ 节日签 pt14–18
#   主区时段 pt17–38 ｜ 失败占位 pt17–30 ｜ 底栏标签 pt23 ｜ 休息 pt72
CASES: list[tuple[str, int, int, str]] = [
    # —— 顶栏 年月日（target_h 27 → pt28）
    ("2026年9月23日", 27, 16, "left"),
    ("1900年1月1日", 27, 16, "left"),
    ("2099年12月31日", 27, 16, "left"),

    # —— 顶栏 农历（无节日：target_h 18–21）
    ("八月十三", 21, 200, "right"),
    ("正月初一", 21, 200, "right"),
    ("闰五月初四", 21, 200, "right"),
    ("腊月三十", 21, 200, "right"),
    ("冬月廿九", 20, 200, "right"),
    ("九月初五", 19, 200, "right"),
    ("二月初二", 18, 200, "right"),

    # —— 顶栏 农历（有节日：target_h 15–18）
    ("八月十五", 18, 200, "right"),
    ("腊月初八", 18, 200, "right"),
    ("八月十三", 17, 200, "right"),
    ("正月初一", 16, 200, "right"),
    ("七月十五", 15, 200, "right"),

    # —— 顶栏 节日反白签（pt14–18）
    ("中秋节", 18, 300, "center"),
    ("国庆节", 16, 300, "center"),
    ("龙抬头", 15, 300, "center"),
    ("除夕", 14, 300, "center"),

    # —— 主区 时段（target_h 18–30 → pt17–38），覆盖单/双位小时与各长度
    ("上午08:00-12:00", 30, 16, "left"),
    ("下午14:00-18:00", 30, 16, "left"),
    ("夜间20:00-24:00", 28, 16, "left"),
    ("凌晨00:00-04:00", 26, 16, "left"),
    ("上午9:30-18:30", 25, 16, "left"),
    ("凌晨06:00-08:00", 24, 16, "left"),
    ("下午8:00-4:00", 22, 16, "left"),
    ("上午08:00-12:00", 20, 16, "left"),
    ("夜间20:00-24:00", 18, 16, "left"),

    # —— 主区 失败占位
    ("获取失败", 30, 118, "center"),
    ("获取失败", 24, 118, "center"),
    ("获取失败", 18, 118, "center"),

    # —— 底栏
    ("今日班次", 23, 16, "left"),
]

# 固定 pt 的整串用例：(文本, pt, 锚点中心 x, 对齐)。
# ⚠️ 「休息」走的是 `text_pt(..., REST_PT=72)` —— **固定 pt，不走墨迹高路径**。
# 若误放进上面的 CASES，会被当成 target_h=72 去二分，算出 pt75 并误报「字库缺字号」。
FIXED_CASES: list[tuple[str, int, int, str]] = [
    ("休息", 72, 118, "center"),
]

# 反白块用例：(班次名, pt)—— 取 `pt_census.SHIFT_KNOWN_FORMS` 的全部**已知形态**，
# 每个形态在降级区间（28→20）的三个代表档位上各验一次。
# 用同一份真源，避免「测试用例」与「字模清单」各自维护而漂移。
REVERSED: list[tuple[str, int]] = [
    (form, pt)
    for form in pt_census.SHIFT_KNOWN_FORMS
    for pt in (28, 24, 20)
] + [("休息", 28), ("休息", 24)]

# 守卫自检用的**必定不存在**的字（确认字符断言真的会报错，而不是恒真）。
GUARD_NEGATIVE = "鑫"

CHARS_BY_PT: dict[int, set[str]] = {}


def missing_chars(text: str, pt: int) -> list[str]:
    """返回 text 在 pt 字号组里**字库没有**的字符（去重、按码点升序）。"""
    have = CHARS_BY_PT.get(pt)
    if have is None:                      # 连这个 pt 都没有 → 全部算缺
        return sorted(set(text))
    return sorted(set(text) - have)


def blit(glyphs: list[tuple[int, dict]], origin: tuple[int, int],
         target: Image.Image, ink_level: int) -> None:
    """按导出的字模数据把整串贴到画布上。"""
    px = target.load()
    width, height = target.size
    for pen, g in glyphs:
        if g["iw"] == 0 or g["ih"] == 0:
            continue
        gx = origin[0] + pen + g["idx"]
        gy = origin[1] + g["idy"]
        stride = (g["iw"] + 7) // 8
        for row in range(g["ih"]):
            y = gy + row
            if y < 0 or y >= height:
                continue
            for col in range(g["iw"]):
                x = gx + col
                if x < 0 or x >= width:
                    continue
                if g["rows"][row * stride + col // 8] & (0x80 >> (col % 8)):
                    px[x, y] = ink_level


def layout(glyphs: list[tuple[int, dict]], align: str, cx: int, cy: int):
    """复刻固件 cal_text_layout 的算法，返回 (度量, 文本原点)。"""
    lefts, tops, rights, bottoms = [], [], [], []
    for pen, g in glyphs:
        if g["bw"] == 0 or g["bh"] == 0:
            continue
        lefts.append(pen + g["bdx"])
        rights.append(pen + g["bdx"] + g["bw"])
        tops.append(g["bdy"])
        bottoms.append(g["bdy"] + g["bh"])
    left, top = min(lefts), min(tops)
    right, bottom = max(rights), max(bottoms)
    width, height = right - left, bottom - top
    x = {"center": cx - width // 2, "left": cx, "right": cx - width}[align]
    return (width, height), (x - left, cy - height // 2 - top)


def glyphs_of(text: str, pt: int) -> list[tuple[int, dict]]:
    out, pen = [], 0
    for ch in text:
        g = font_bitmap.glyph_metrics(ch, pt)
        out.append((pen, g))
        pen += g["advance"]
    return out


def simulate_text(text: str, pt: int, align: str, cx: int, cy: int) -> Image.Image:
    img = Image.new("L", CANVAS, 255)
    glyphs = glyphs_of(text, pt)
    _metrics, origin = layout(glyphs, align, cx, cy)
    blit(glyphs, origin, img, 0)
    return img


def simulate_reversed(text: str, pt: int, cx: int, cy: int, box_h: int,
                      pad_x: int) -> Image.Image:
    img = Image.new("L", CANVAS, 255)
    glyphs = glyphs_of(text, pt)
    metrics, origin = layout(glyphs, "center", cx, cy)
    box_w = metrics[0] + pad_x * 2
    x0 = cx - box_w // 2
    y0 = cy - box_h // 2
    draw = img.load()
    for y in range(max(0, y0), min(CANVAS[1], y0 + box_h)):
        for x in range(max(0, x0), min(CANVAS[0], x0 + box_w)):
            draw[x, y] = 0
    blit(glyphs, origin, img, 255)
    return img


def diff(a: Image.Image, b: Image.Image) -> int:
    ba = a.point(lambda p: 255 if p <= 127 else 0, "1").tobytes()
    bb = b.point(lambda p: 255 if p <= 127 else 0, "1").tobytes()
    return sum(1 for x, y in zip(ba, bb) if x != y)


def plan_pts() -> set[int]:
    return {pt for pt, _chars in font_bitmap.load_plan()}


def verify_all_glyphs() -> int:
    """每个字号组里的每个字，逐个比对居中/左齐/右齐三种摆放。

    这里遍历的就是字库清单本身，故**字符存在性天然成立**，只验几何模型。
    """
    bad = 0
    checked = 0
    for pt, chars in font_bitmap.load_plan():
        for ch in chars:
            for align in ("center", "left", "right"):
                ref = cp.Canvas()
                ref.text_pt(ch, cp.FONT_FILE, pt, 200, 150, align=align)
                sim = simulate_text(ch, pt, align, 200, 150)
                checked += 1
                if diff(ref.img, sim):
                    bad += 1
                    print(f"  不一致：pt{pt}「{ch}」align={align}")
    print(f"逐字比对（{len(CHARS_BY_PT)} 组）：{checked} 例，不一致 {bad} 例")
    return bad


def verify_strings(pts: set[int]) -> int:
    """整串比对，断言 pt 在字库内 **且每个字符在字库内**。"""
    bad = 0
    missing: list[str] = []
    print(f"{'字符串':<18}{'h':>4}{'pt':>4}{'居中':>7}{'左齐':>7}{'右齐':>7}")
    print("-" * 54)
    for text, target_h, _cx, align in CASES:
        c = cp.Canvas()
        pt = c.font_for_height(text, cp.FONT_FILE, target_h).size
        if pt not in pts:
            missing.append(f"「{text}」h={target_h} → pt{pt}（整个字号组都缺）")
            bad += 1
            print(f"{text:<18}{target_h:>4}{pt:>4}   ❌ 字库缺此行")
            continue
        gone = missing_chars(text, pt)
        if gone:
            missing.append(f"「{text}」pt{pt} → 缺字 {''.join(gone)}")
            bad += 1
            print(f"{text:<18}{target_h:>4}{pt:>4}   ❌ 缺字 {''.join(gone)}")
            continue
        cells = []
        for a in ("center", "left", "right"):
            ref = cp.Canvas()
            ref.text_pt(text, cp.FONT_FILE, pt, 200, 150, align=a)
            d = diff(ref.img, simulate_text(text, pt, a, 200, 150))
            cells.append(d)
            bad += 1 if d else 0
        mark = "  " if align == "center" else f"  ({align} 为实绘)"
        print(f"{text:<18}{target_h:>4}{pt:>4}" + "".join(f"{v:>7}" for v in cells) + mark)
    print("-" * 54)
    print(f"整串比对：{len(CASES) * 3} 例，不一致 {bad} 例")
    if missing:
        print("❌ 字库缺字号 / 缺字（普查清单不全）：")
        for m in missing:
            print("   " + m)
    return bad


def verify_reversed(pts: set[int]) -> int:
    """反白班次块：黑底白字；覆盖**全部已知班次名形态** × 降级区间代表档位。

    班次名来自第三方数据源、**不可穷举**，所以这里是唯一能把「括号内备注换了个新词」
    变成红灯的地方 —— 必须断言到**字**一级。
    """
    bad = 0
    box_h = 52
    pad = 18
    print(f"{'班次名':<14}{'pt':>4}{'墨迹宽':>8}{'差异':>8}")
    print("-" * 36)
    for shift, pt in REVERSED:
        if pt not in pts:
            bad += 1
            print(f"{shift:<14}{pt:>4}      ❌ 字库缺此 pt")
            continue
        gone = missing_chars(shift, pt)
        if gone:
            bad += 1
            print(f"{shift:<14}{pt:>4}      ❌ 缺字 {''.join(gone)}")
            continue
        ref = cp.Canvas()
        ref.reversed_box_pt(shift, cp.FONT_FILE, pt, 200, 150, box_h, pad_x=pad)
        sim = simulate_reversed(shift, pt, 200, 150, box_h, pad)
        d = diff(ref.img, sim)
        bad += 1 if d else 0
        print(f"{shift:<14}{pt:>4}"
              f"{ref.text_width_pt(shift, cp.FONT_FILE, pt):>8}{d:>8}")
    print("-" * 36)
    print(f"反白块比对：{len(REVERSED)} 例，不一致 {bad} 例")
    return bad


def verify_shift_vocabulary(pts: set[int]) -> int:
    """班次名**词汇守卫**：全部已知形态的每个字，必须在 pt20–28 每一档都在。

    这一条与 `pt_census.shift_charset()` 是**双向**的：
    清单漏了字 → 这里报缺字；加了字却没人用 → 由 `pt_census` 的冗余检查兜住。
    """
    bad = 0
    need = set("".join(pt_census.SHIFT_KNOWN_FORMS))
    print(f"已知班次名形态 {len(pt_census.SHIFT_KNOWN_FORMS)} 种，"
          f"去重用字 {len(need)} 个：{''.join(sorted(need))}")
    for pt in range(cp.SHIFT_PT_MIN, cp.SHIFT_PT + 1):
        if pt not in pts:
            bad += 1
            print(f"  pt{pt}  ❌ 整个字号组不在字库内")
            continue
        gone = missing_chars("".join(pt_census.SHIFT_KNOWN_FORMS), pt)
        if gone:
            bad += 1
            print(f"  pt{pt}  ❌ 缺字 {''.join(gone)}")
    print(f"词汇守卫：pt{cp.SHIFT_PT_MIN}–pt{cp.SHIFT_PT} 共 "
          f"{cp.SHIFT_PT - cp.SHIFT_PT_MIN + 1} 档，{bad} 档不合格")
    return bad


def verify_fixed(pts: set[int]) -> int:
    """固定 pt 的**整串**绘制（pt 由常量给出，不经墨迹高路径）。

    逐字绘制的固定 pt 区域（星期 pt44 / 节气 pt31）已由 `verify_all_glyphs` 覆盖，
    这里只补「一次画整串」的那处（休息 pt72）。
    """
    bad = 0
    print(f"{'字符串':<14}{'pt':>4}{'居中':>7}{'左齐':>7}{'右齐':>7}")
    print("-" * 40)
    for text, pt, cx, _align in FIXED_CASES:
        if pt not in pts:
            bad += 1
            print(f"{text:<14}{pt:>4}   ❌ 字库缺此 pt")
            continue
        gone = missing_chars(text, pt)
        if gone:
            bad += 1
            print(f"{text:<14}{pt:>4}   ❌ 缺字 {''.join(gone)}")
            continue
        cells = []
        for a in ("center", "left", "right"):
            ref = cp.Canvas()
            ref.text_pt(text, cp.FONT_FILE, pt, cx, 150, align=a)
            d = diff(ref.img, simulate_text(text, pt, a, cx, 150))
            cells.append(d)
            bad += 1 if d else 0
        print(f"{text:<14}{pt:>4}" + "".join(f"{v:>7}" for v in cells))
    print("-" * 40)
    print(f"固定 pt 整串比对：{len(FIXED_CASES) * 3} 例，不一致 {bad} 例")
    return bad


def verify_guard() -> int:
    """守卫自检：确认 `missing_chars()` 真会报错，而不是恒返回空表。

    一个恒真的断言比没有断言更糟 —— 它会让人以为已经守住了。
    """
    bad = 0
    if GUARD_NEGATIVE in "".join("".join(chars) for _pt, chars in font_bitmap.load_plan()):
        print(f"⚠️  守卫自检用的「{GUARD_NEGATIVE}」已进字库，请换一个必定不存在的字")
        return 1
    for pt in (28, 20):                    # 字库内 / 字库外 两条路径都要红
        gone = missing_chars(GUARD_NEGATIVE, pt)
        if gone != [GUARD_NEGATIVE]:
            print(f"❌ 守卫自检失败：pt{pt}「{GUARD_NEGATIVE}」未被识别为缺字 → {gone}")
            bad += 1
    ok = "通过" if not bad else "失败"
    print(f"守卫自检：{ok}（「{GUARD_NEGATIVE}」在 pt28/pt20 均被正确判为缺字）")
    return bad


def main() -> None:
    global CHARS_BY_PT
    CHARS_BY_PT = {pt: set(chars) for pt, chars in font_bitmap.load_plan()}
    pts = set(CHARS_BY_PT)
    print(f"字库字号组：{len(pts)} 个 → {sorted(pts)}\n")
    bad = 0
    bad += verify_all_glyphs()
    print()
    bad += verify_strings(pts)
    print()
    bad += verify_fixed(pts)
    print()
    bad += verify_reversed(pts)
    print()
    bad += verify_shift_vocabulary(pts)
    print()
    bad += verify_guard()
    print()
    if bad:
        raise SystemExit(f"字模数据与渲染器不一致 / 字库缺字（{bad} 例），需要检查生成器")
    print("全部一致：导出的字模数据可以在固件侧复现渲染器的绘制结果。")


if __name__ == "__main__":
    main()
