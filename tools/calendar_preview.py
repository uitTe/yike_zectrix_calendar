"""台历版式预览生成器。

按 Zectrix Note4 墨水屏的真实规格出图：400x300、纯黑白（1bpp、无灰阶）。
1bpp 的关键约束：不能有抗锯齿，所有像素必须硬阈值二值化 —— 小字号中文会糊，
所以中文字号下限控制在 20px 以上。

产出 3 个版式变式，供挑选后再生成固件字模。
"""

from __future__ import annotations

import argparse
import datetime as dt
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

W, H = 400, 300

# --- 字体 -------------------------------------------------------------------
#
# **开源可商用**：Noto Sans SC（= Google 版思源黑体），SIL OFL 1.1 授权。
# ⚠️ 刻意不用 Windows 自带的微软雅黑 / 等线 —— 把字形渲染成点阵**嵌进固件**
#    属于字体的「复制 / 嵌入」行为，商用需授权。Noto / Source Han 走 OFL，无此约束。
#    另一个好处：数字与中文同字体，字号/比例天然一致。
#
# 这是**可变字体**（Weight 轴 100~900）。系统里这份的**默认实例是极细的 100**，
# 不显式调轴就会渲染成 Thin —— 18px 小字二值化后笔画会断
# （实测 18px「中秋节」：默认实例 155 黑像素 vs wght700 的 362）。
# 故一律经 load_font() 取字体，由它统一设 wght=700。
FONT_FILE = r"C:\Windows\Fonts\NotoSansSC-VF.ttf"
FONT_WEIGHT = 700
F_CN = FONT_FILE       # 中文 + 数字 + 字母（统一一个字体，避免混排比例失调）
F_DIGIT = FONT_FILE    # 超大日号（仅 B/C 版式用）也走同一个

BLACK, WHITE = 0, 255

_FONT_CACHE: dict[tuple[str, int], ImageFont.FreeTypeFont] = {}


def load_font(path: str, pt: int) -> ImageFont.FreeTypeFont:
    """按 (字体文件, pt) 取字体，对可变字体统一应用 Weight 轴。带缓存。

    ⚠️ 不要绕开这个函数直接 `ImageFont.truetype()` —— 那样会拿到 VF 的默认极细实例。
    """
    key = (path, pt)
    f = _FONT_CACHE.get(key)
    if f is None:
        f = ImageFont.truetype(path, pt)
        if path == FONT_FILE:
            try:
                f.set_variation_by_axes([FONT_WEIGHT])
            except Exception:      # 非可变字体则忽略
                pass
        _FONT_CACHE[key] = f
    return f

# A 版骨架的几何常量。改版式只改这里 —— 定稿渲染器 calendar_A_final.py
# 也引用同一组值（节气列要贴着星期列排），避免两处硬编码各自漂移。
DIV_X = 232          # 主区左右分栏的竖线 x（顶栏/底栏分隔线的右端同此）
# 竖排星期 / 节气都是**逐字**绘制，必须用固定 pt，不能按墨迹高度二分 ——
# 「一」只有一根横画，它的"墨迹高度"其实是笔画**厚度**，为了凑"高 40"字号会被一路
# 顶到 **pt95**（横杠 95px 宽，是其他字的两倍多）；「二」pt53、「三」pt48 同理。
# 改为固定 pt 后各字宽度统一（星期 44px / 节气 31px），竖排才真正对齐。
WEEK_CX = 316        # 竖排星期：列中心
WEEK_PT = 44         # 竖排星期：字号（pt44 → 单字墨迹宽 44、高 37~44）
WEEK_TOP = 104       # 第一字的垂直中心
WEEK_STEP = 52       # 相邻字的中心距
REST_PT = 72         # 休息日主区「休息」二字字号（居中于左栏）
FAIL_TEXT = "获取失败"  # 拉取失败且无可用缓存时，主区左栏的占位文案（替代时段两行）

# 底栏反白班次块。**班次名来自第三方数据源，长度与形态都不可控**，实测已见：
#     `9DP1` `9NP1`            纯代码
#     `9DP(绿)` `9DP(白)`      代码 + 颜色备注
#     `9DP0(验车)`             代码 + 中文备注  ← 最长，pt28 墨迹 152px
#     `休`                     单字（休息日）
#     `9DP1(绿)`               代码 + 颜色 + 序号
#
# 基准取 **pt28**（而不是"墨迹高 34"）：数字/字母墨迹只占 em 的 ~71%，
# 若按墨迹高二分会把 `9DP1` 选到 pt44（墨迹宽 118px，撑爆底栏）—— 故一律以 pt 为准。
#
# 三重兜底：
#   ① `normalize_shift()` 全角→半角（`9DP0（验车）` 141→124px）
#   ② 超 SHIFT_BOX_MAX 就逐级降 pt（下限 20px）
#   ③ 块仍越界时由 `shift_box_cx()` 平移中心（左不撞标签、右不出屏）
SHIFT_CX = 320        # 块中心基准（≙ 定稿 2 字班次 x[265,374]，小块位置不变）
SHIFT_CY = 266
SHIFT_PT = 28         # 基准字号
SHIFT_PT_MIN = 20     # 收缩下限（1bpp 中文可读下限）
SHIFT_PAD_X = 18
SHIFT_BOX_H = 52      # 固定块高 → 底栏观感不随班次名的胖瘦变化
SHIFT_BOX_MAX = 200   # 块宽**软**上限：超过则收缩字号
SHIFT_LEFT_MIN = 120  # 块左边界**硬**下限：左标签「今日班次」墨迹右界 106 + 14px 间隙
SHIFT_RIGHT_MAX = 392 # 块右边界**硬**上限：右边距 5px


class Canvas:
    """400x300 单色画布。坐标原点在左上角。"""

    def __init__(self) -> None:
        self.img = Image.new("L", (W, H), WHITE)
        self.d = ImageDraw.Draw(self.img)

    def font_for_height(self, text: str, path: str, target_h: int) -> ImageFont.FreeTypeFont:
        """二分搜索字号，使文本实际像素高度最接近 target_h。"""
        lo, hi, best = 6, 400, load_font(path, 6)
        while lo <= hi:
            mid = (lo + hi) // 2
            f = load_font(path, mid)
            b = self.d.textbbox((0, 0), text, font=f)
            if (b[3] - b[1]) <= target_h:
                best = f
                lo = mid + 1
            else:
                hi = mid - 1
        return best

    def text(self, text: str, path: str, target_h: int, cx: int, cy: int,
             align: str = "center") -> tuple[int, int]:
        """以 (cx, cy) 为锚点居中绘制文本（按实际墨迹包围盒对齐，不是按字体基线）。

        align: center / left / right —— 决定 cx 对应的水平锚点。
        返回实际墨迹宽度与高度。
        """
        f = self.font_for_height(text, path, target_h)
        b = self.d.textbbox((0, 0), text, font=f)
        tw, th = b[2] - b[0], b[3] - b[1]
        x = {"center": cx - tw // 2, "left": cx, "right": cx - tw}[align]
        # textbbox 的上边界可能非 0，做减除才能真正垂直居中
        self.d.text((x - b[0], cy - th // 2 - b[1]), text, font=f, fill=BLACK)
        return tw, th

    def text_width(self, text: str, path: str, target_h: int) -> int:
        f = self.font_for_height(text, path, target_h)
        b = self.d.textbbox((0, 0), text, font=f)
        return b[2] - b[0]

    def hline(self, y: int, x0: int = 0, x1: int = W, thickness: int = 1) -> None:
        self.d.rectangle([x0, y, x1 - 1, y + thickness - 1], fill=BLACK)

    def vline(self, x: int, y0: int = 0, y1: int = H, thickness: int = 1) -> None:
        self.d.rectangle([x, y0, x + thickness - 1, y1 - 1], fill=BLACK)

    def box(self, x0: int, y0: int, x1: int, y1: int, fill: int = BLACK,
            outline: bool = False) -> None:
        if fill == BLACK and not outline:
            self.d.rectangle([x0, y0, x1 - 1, y1 - 1], fill=BLACK)
        else:
            self.d.rectangle([x0, y0, x1 - 1, y1 - 1], outline=BLACK, width=2)

    def reversed_text(self, text: str, path: str, target_h: int, cx: int, cy: int,
                      pad_x: int = 10, pad_y: int = 6) -> None:
        """反白文字：黑底白字。墨水屏上反差最强，适合突出班次。"""
        f = self.font_for_height(text, path, target_h)
        b = self.d.textbbox((0, 0), text, font=f)
        tw, th = b[2] - b[0], b[3] - b[1]
        x0 = cx - tw // 2 - pad_x
        y0 = cy - th // 2 - pad_y
        self.d.rectangle([x0, y0, x0 + tw + pad_x * 2 - 1, y0 + th + pad_y * 2 - 1], fill=BLACK)
        self.d.text((x0 + pad_x - b[0], y0 + pad_y - b[1]), text, font=f, fill=WHITE)

    # --- 按「字号 pt」绘制 ---------------------------------------------------
    # ⚠️ 混排（拉丁 + 中文）必须走这一组，别用上面的 target_h 版：
    # target_h 是按**墨迹高度**二分选字号，而数字/字母的墨迹只占 em 的 ~71%，
    # 于是纯拉丁会被选到过大的 pt —— 实测 `9DP1` 若要求"墨迹高 34"会选到 **pt44**，
    # 墨迹宽 118px，直接把底栏撑爆。班次名正是这种混合内容，故以 pt 为准。

    def text_pt(self, text: str, path: str, pt: int, cx: int, cy: int,
                align: str = "center", fill: int = BLACK) -> tuple[int, int]:
        f = load_font(path, pt)
        b = self.d.textbbox((0, 0), text, font=f)
        tw, th = b[2] - b[0], b[3] - b[1]
        x = {"center": cx - tw // 2, "left": cx, "right": cx - tw}[align]
        self.d.text((x - b[0], cy - th // 2 - b[1]), text, font=f, fill=fill)
        return tw, th

    def text_width_pt(self, text: str, path: str, pt: int) -> int:
        f = load_font(path, pt)
        b = self.d.textbbox((0, 0), text, font=f)
        return b[2] - b[0]

    def reversed_box_pt(self, text: str, path: str, pt: int, cx: int, cy: int,
                        box_h: int, pad_x: int = 18) -> tuple[int, int]:
        """反白块：**固定高度**、宽度随内容，文字在块内垂直居中。

        固定块高是有理由的：不同班次名的墨迹高差异很大（`9DP1` 全拉丁、高仅
        21px；纯中文则 28px），块高若跟着走，底栏就会一天胖一天瘦。
        """
        f = load_font(path, pt)
        b = self.d.textbbox((0, 0), text, font=f)
        tw, th = b[2] - b[0], b[3] - b[1]
        box_w = tw + pad_x * 2
        x0 = cx - box_w // 2
        y0 = cy - box_h // 2
        self.d.rectangle([x0, y0, x0 + box_w - 1, y0 + box_h - 1], fill=BLACK)
        self.d.text((x0 + pad_x - b[0], cy - th // 2 - b[1]), text, font=f, fill=WHITE)
        return box_w, box_h

    def save_1bpp(self, path: Path) -> None:
        """硬阈值二值化 —— 模拟墨水屏不可显示灰阶的真实观感。"""
        bw = self.img.point(lambda p: 255 if p > 127 else 0, "1")
        bw.save(path)
        # 2 倍最近邻放大版，便于在屏幕上细看像素
        bw.convert("L").resize((W * 2, H * 2), Image.NEAREST).save(
            path.with_name(path.stem + "@2x.png"))


def normalize_shift(s: str) -> str:
    """班次名归一化：全角 → 半角、去首尾空白。

    第三方数据源的格式不统一，实测同一班组会写成 `9DP（绿）`（全角括号）或
    `9DP(绿)`（半角）。全角括号把宽度从 107px 顶到 141px → 被迫缩字号。
    归一化一步解决，白省 34px。顺带覆盖全角数字/字母/空格。
    """
    out = []
    for ch in s.strip():
        o = ord(ch)
        if o == 0x3000:                    # 全角空格
            out.append(" ")
        elif 0xFF01 <= o <= 0xFF5E:        # 全角 ASCII（! 到 ~）
            out.append(chr(o - 0xFEE0))
        else:
            out.append(ch)
    return "".join(out)


def is_rest(shift: str) -> bool:
    """休息日判定：班次名归一化后含「休」字。

    实测形态是单个 `休`；放宽到"包含"是为了兼容 `休息` / `休假` / `休班` 这类写法。
    休息日**不画时段两行**，主区左栏直接显示「休息」大字。
    """
    return "休" in normalize_shift(shift)


def shift_box_cx(box_w: int) -> int:
    """反白块的中心 x：优先锚定 SHIFT_CX，过宽时平移，保证既不撞左标签也不出屏。

    小块（`休` 64px、`9DP1` 110px）算出来就是 SHIFT_CX，位置与定稿一致；
    只有 `9DP0(验车)` 这类长名才会左移。实测块宽 276px 仍能放下。
    """
    cx = SHIFT_CX
    if cx + (box_w + 1) // 2 > SHIFT_RIGHT_MAX:          # 右边界越界 → 左移
        cx = SHIFT_RIGHT_MAX - (box_w + 1) // 2
    if cx - box_w // 2 < SHIFT_LEFT_MIN:                  # 左边界越界 → 右移（左标签优先）
        cx = SHIFT_LEFT_MIN + box_w // 2
    return cx


def variant_a(c: Canvas, m: dict) -> None:
    """变式 A（已定稿）：左右分栏。

    顶部：完整年月日（不再显示周次）
    主区左：上午 / 下午 两行班次时段
    主区右：竖排星期（保持不变）
    底部：今日班次 + 反白班次名（保持不变）
    """
    # 顶栏：完整年月日
    c.text(m["date_cn"], F_CN, 27, 16, 30, align="left")
    c.hline(52, thickness=2)

    # 主区分隔竖线 + 右侧星期（竖排三字，固定 pt → 各字等宽）
    c.vline(DIV_X, 62, DIV_X, thickness=2)
    for i, ch in enumerate(m["weekday_cn"]):
        c.text_pt(ch, F_CN, WEEK_PT, WEEK_CX, WEEK_TOP + i * WEEK_STEP)

    # 主区左：四态 —— blank_schedule（该天无排班，整栏留白）/ 休息日 /
    # 拉取失败 / 正常时段。无排班与拉取失败都表现为内容为空，由调用方置
    # blank_schedule 区分，渲染层不靠"是否为空"猜是哪一种。
    x0, x1 = 16, 220          # 左边界与顶栏标题对齐
    y_mid = (62 + 232) // 2
    am, pm = m["am"].strip(), m["pm"].strip()
    blank_schedule = m.get("blank_schedule", False)
    if not blank_schedule and is_rest(m["shift"]):
        c.text_pt("休息", F_CN, REST_PT, (x0 + x1) // 2, y_mid)
    elif not blank_schedule and not am and not pm:
        # 时段两行都空 = 没拿到数据（拉取失败且无可用缓存）。
        # 数据缺失时**不能**显示"上午""下午"两个光秃秃的前缀 —— 那是空文本被拼了前缀。
        h = 30
        while h > 18 and c.text_width(FAIL_TEXT, F_CN, h) > (x1 - x0):
            h -= 1
        c.text(FAIL_TEXT, F_CN, h, (x0 + x1) // 2, y_mid)
    elif not blank_schedule:
        lines = [t for t in (am, pm) if t]      # 只有一段数据就只画一行，不留空行
        h = 30
        while h > 18 and max(c.text_width(t, F_CN, h) for t in lines) > (x1 - x0):
            h -= 1
        gap = 60
        top = y_mid - (len(lines) - 1) * gap // 2
        for i, t in enumerate(lines):
            c.text(t, F_CN, h, x0, top + i * gap, align="left")

    # 底栏：今日班次 + 反白班次名。
    # 班次名来自数据源（代码式，如 `9DP1` / `9DP1(绿)`），长度不可控 →
    # 先归一化全角，再按 pt 收缩，最后画在**固定高度**的反白块里。
    c.hline(DIV_X, thickness=2)
    c.text("今日班次", F_CN, 23, 16, 266, align="left")
    shift = normalize_shift(m["shift"])
    if not blank_schedule and shift:
        pt = SHIFT_PT
        while pt > SHIFT_PT_MIN and c.text_width_pt(shift, F_CN, pt) + 2 * SHIFT_PAD_X > SHIFT_BOX_MAX:
            pt -= 1
        box_w = c.text_width_pt(shift, F_CN, pt) + 2 * SHIFT_PAD_X
        c.reversed_box_pt(shift, F_CN, pt, shift_box_cx(box_w), SHIFT_CY, SHIFT_BOX_H,
                          pad_x=SHIFT_PAD_X)


def variant_b(c: Canvas, m: dict) -> None:
    """变式 B：上日期下横幅。日期居中放大，班次做成通栏反白横幅。"""
    c.text(f"{m['year']} · {m['month']:02d} · {m['day']:02d}", F_CN, 20, W // 2, 26)

    # 超大日号居中。注意留出下方星期的位置，否则会和数字下沿撞上
    c.text(str(m["day"]), F_DIGIT, 128, W // 2, 120)

    # 星期横排
    c.text(m["weekday_cn"], F_CN, 30, W // 2, 217)

    # 通栏反白班次横幅（黑底白字，反差最强）
    c.box(0, 246, W, H)
    c_text_white(c, "今日班次", F_CN, 20, 20, 273, "left")
    c_text_white(c, m["shift"], F_CN, 38, W - 20, 273, "right")


def c_text_white(c: Canvas, text: str, path: str, target_h: int, cx: int, cy: int,
                 align: str = "center") -> None:
    """在黑底上以白色重绘文本。"""
    f = c.font_for_height(text, path, target_h)
    b = c.d.textbbox((0, 0), text, font=f)
    tw, th = b[2] - b[0], b[3] - b[1]
    x = {"center": cx - tw // 2, "left": cx, "right": cx - tw}[align]
    c.d.text((x - b[0], cy - th // 2 - b[1]), text, font=f, fill=WHITE)


def variant_c(c: Canvas, m: dict) -> None:
    """变式 C：极简居中。信息最少的三个元素，字最大、最干净。"""
    c.text(f"{m['year']} 年 {m['month']} 月", F_CN, 22, W // 2, 26)
    c.hline(46, thickness=1)

    c.text(str(m["day"]), F_DIGIT, 165, W // 2, 142)

    c.hline(238, thickness=1)
    c.text(m["weekday_cn"], F_CN, 30, 76, 269)
    c.reversed_text(m["shift"], F_CN, 30, 300, 269, pad_x=16, pad_y=8)


VARIANTS = {
    "A": ("左右分栏 · 左侧上午/下午时段 + 右竖排星期", variant_a),
    "B": ("上下分区 · 居中数字 + 通栏反白班次", variant_b),
    "C": ("极简居中 · 字最大最干净", variant_c),
}


def main() -> None:
    ap = argparse.ArgumentParser(description="生成台历版式预览图（400x300 1bpp）")
    ap.add_argument("--out", default="D:/zectrix/zectrix_yike/design",
                    help="输出目录")
    ap.add_argument("--date", default="2026-09-23", help="预览用日期 YYYY-MM-DD")
    ap.add_argument("--shift", default="早班", help="预览用班次")
    ap.add_argument("--am", default="上午08:00-12:00", help="上午时段（mock）")
    ap.add_argument("--pm", default="下午14:00-18:00", help="下午时段（mock）")
    ap.add_argument("--only", default=None, help="只渲染指定变式（A/B/C），默认全部")
    args = ap.parse_args()

    d = dt.date.fromisoformat(args.date)
    wd = "星期" + "一二三四五六日"[d.weekday()]
    meta = {
        "year": d.year, "month": d.month, "day": d.day,
        "week": d.isocalendar().week, "weekday_cn": wd, "shift": args.shift,
        "date_cn": f"{d.year}年{d.month}月{d.day}日",
        "am": args.am, "pm": args.pm,
    }

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    print(f"预览数据：{d} {wd} · {args.shift} · {args.am} / {args.pm}")
    print()
    for key, (desc, fn) in VARIANTS.items():
        if args.only and key != args.only.upper():
            continue
        c = Canvas()
        fn(c, meta)
        p = out / f"calendar_variant_{key}.png"
        c.save_1bpp(p)
        print(f"  变式 {key}  {desc}")
        print(f"           {p}")
    print()
    print("每张同时输出 @2x 放大版，便于细看像素。")


if __name__ == "__main__":
    main()
