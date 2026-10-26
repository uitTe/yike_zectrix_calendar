"""A 版台历 + 农历 / 节日：三种落位方案对比。

直接复用 `calendar_preview.variant_a` 画出**已定稿的 A 版骨架**（单一真源，
不会和定稿版本跑偏），只在其上叠加「农历 + 节日」的三种摆法。

方案 1：顶栏右侧单行农历；节日做成**底栏中部反白小签**
方案 2：顶栏右侧**两行** —— 上农历、下反白节日签（底栏完全不动）
方案 3：顶栏右侧**单行** —— 农历 + 反白节日签并排（底栏完全不动）

无节日时三种方案都只显示农历，且农历位置不变 → 布局不跳动。

⚠️ **这是选型阶段的历史脚本，已被 `calendar_A_final.py` 取代。**
   它自带一份顶栏实现（仍是**单节日**逻辑），所以：
   - 它的产物**不能**当权威图看，多节日撞车日会少画一个签；
   - 它的输出文件名统一以 `stage_lunar_` 开头，不要再占用 `calendar_A_final_*`
     这个前缀（那个前缀属于权威定稿渲染器，见 design/README 或 docs/03）。
"""

from __future__ import annotations

import argparse
import datetime as dt
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from calendar_preview import Canvas, F_CN, F_DIGIT, W, H  # noqa: E402
import lunar_cn  # noqa: E402

# A 版骨架的既有坐标（与 calendar_preview.variant_a 保持一致）
TOP_TITLE_H = 27
TOP_TITLE_X = 16
MAIN_Y0, MAIN_Y1 = 62, 232
BOT_CY = 266
RIGHT_PAD = 16
DIVIDER_X = 232


def tag_left(c: Canvas, text: str, h: int, x_left: int, cy: int,
             pad_x: int = 11, pad_y: int = 6) -> int:
    """反白标签，按**左边界**对齐（Canvas.reversed_text 是按中心锚的）。

    返回标签总宽度。
    """
    tw = c.text_width(text, F_CN, h)
    total = tw + pad_x * 2
    c.reversed_text(text, F_CN, h, x_left + total // 2, cy, pad_x=pad_x, pad_y=pad_y)
    return total


def opt1(c: Canvas, m: dict, right_x: int) -> None:
    """农历右对齐在顶栏；节日 → 底栏中部反白小签。"""
    c.text(m["lunar_cn"], F_CN, 21, right_x, 30, align="right")

    if not m["festival"]:
        return
    # 底栏中部：今日班次(左) 与 班次反白块(右) 之间的空档
    span_l, span_r = 116, 262
    label = m["festival"]
    cx = (span_l + span_r) // 2
    c.reversed_text(label, F_CN, 21, cx, BOT_CY, pad_x=12, pad_y=7)


def opt2(c: Canvas, m: dict, right_x: int) -> None:
    """顶栏右侧两行：上农历、下反白节日签；底栏不动。

    无节日时农历单独占顶栏右侧、与标题同高（h21 / cy30），观感最优；
    有节日时才升到两行（h18 / cy17 + 签 cy38）。每次只显示一天，按当天最优排即可。
    """
    if not m["festival"]:
        c.text(m["lunar_cn"], F_CN, 21, right_x, 30, align="right")
        return

    c.text(m["lunar_cn"], F_CN, 18, right_x, 17, align="right")
    tw = c.text_width(m["festival"], F_CN, 18)
    total = tw + 20
    c.reversed_text(m["festival"], F_CN, 18, right_x - total // 2, 38,
                    pad_x=10, pad_y=4)


def opt3(c: Canvas, m: dict, right_x: int) -> None:
    """顶栏右侧单行：农历 + 反白节日签并排，整组右对齐；底栏不动。"""
    if not m["festival"]:
        c.text(m["lunar_cn"], F_CN, 21, right_x, 30, align="right")
        return

    h = 18
    tw_f = c.text_width(m["festival"], F_CN, h)
    tag_w = tw_f + 20
    gap = 7
    tw_l = c.text_width(m["lunar_cn"], F_CN, h)
    lunar_left = right_x - tag_w - gap - tw_l

    c.text(m["lunar_cn"], F_CN, h, lunar_left, 30, align="left")
    c.reversed_text(m["festival"], F_CN, h, right_x - tag_w // 2, 30,
                    pad_x=10, pad_y=4)


def opt4(c: Canvas, m: dict, right_x: int) -> None:
    """（已淘汰，仅留档）农历在顶栏右；节日 → 主区左栏时段下方。

    淘汰原因：反白签上沿离「下午14:00-18:00」的墨迹下沿只剩 5 px，视觉上粘连。
    """
    c.text(m["lunar_cn"], F_CN, 21, right_x, 30, align="right")

    if not m["festival"]:
        return
    tag_left(c, m["festival"], 20, 16, 210, pad_x=12, pad_y=6)


# 方案 2 已由大人于 2026-09-23 定稿 → 作为正式产出
CHOSEN = "2"

OPTIONS = {
    "1": ("农历(顶栏右) + 节日(底栏中部反白签)", opt1),
    "2": ("★已定稿★ 顶栏右侧两行 · 上农历 / 下反白节日签", opt2),
    "3": ("顶栏右侧单行 · 农历 + 反白节日签并排", opt3),
}


def render(d: dt.date, shift: str, am: str, pm: str) -> dict:
    m = lunar_cn.day_info(d)
    m["shift"], m["am"], m["pm"] = shift, am, pm

    c = Canvas()
    title_w = c.text_width(m["date_cn"], F_CN, TOP_TITLE_H)
    right_x = W - RIGHT_PAD

    print(f"{d} {m['weekday_cn']} 农历 {m['lunar_cn']} 节日 {m['festival'] or '—'}")
    print(f"  顶栏标题宽 {title_w}px（x {TOP_TITLE_X}..{TOP_TITLE_X + title_w}）"
          f"  右侧可用 {right_x - (TOP_TITLE_X + title_w + 12)}px")

    out = {}
    for key, (desc, fn) in OPTIONS.items():
        cc = Canvas()
        from calendar_preview import variant_a
        variant_a(cc, m)
        fn(cc, m, right_x)
        out[key] = cc
        print(f"  方案 {key}  {desc}")
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description="A 版台历 + 农历/节日的落位方案对比")
    ap.add_argument("--out", default="D:/zectrix/zectrix_yike/design")
    ap.add_argument("--date", default="2026-09-23")
    ap.add_argument("--date-fest", default="2026-09-25",
                    help="额外渲染一个「有节日」的日期，默认中秋节")
    ap.add_argument("--shift", default="早班")
    ap.add_argument("--am", default="上午08:00-12:00")
    ap.add_argument("--pm", default="下午14:00-18:00")
    ap.add_argument("--only", default=None)
    args = ap.parse_args()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    for date_s, suffix in ((args.date, "plain"), (args.date_fest, "fest")):
        d = dt.date.fromisoformat(date_s)
        print(f"\n--- {suffix}  {date_s} ---")
        canvases = render(d, args.shift, args.am, args.pm)
        for key, cc in canvases.items():
            if args.only and key != args.only:
                continue
            p = out / f"calendar_A_lunar{key}_{suffix}.png"
            cc.save_1bpp(p)
            print(f"      -> {p.name}")
            # 定稿方案的副本。⚠️ 前缀刻意**不再用** `calendar_A_final_` ——
            # 那个前缀归权威渲染器（它出的是 `calendar_A_final_<日期>.png`），
            # 早先这里也叫 `calendar_A_final_fest.png`，两张图同前缀极易看错。
            if key == CHOSEN:
                fp = out / f"stage_lunar_{suffix}.png"
                cc.save_1bpp(fp)
                print(f"      -> {fp.name}   ← 选型阶段副本（非权威图）")

    print("\n@2x 放大版同时输出。方案 1/2/3 对应上面三条描述。")
    print("⚠️ 本脚本是选型阶段历史产物，权威版式请看 calendar_A_final.py 的输出。")


if __name__ == "__main__":
    main()
