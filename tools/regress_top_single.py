"""回归比对：顶栏从「单签」改「多签」后，**单节日那天的像素有没有被动过**。

定稿图是权威。改绘制方法（`reversed_text` → `reversed_box_pt`）有可能悄悄挪 1~2px，
所以这里把**旧实现原样复制一份**，重放同一天，和当前实现逐像素 diff。

判定：
    差异 0  → 单节日外观完全没变，多签是纯增量
    差异 >0 → 打印差异 bbox，人工确认是否可接受（并把结论写进 docs）
"""

from __future__ import annotations

import datetime as dt
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import calendar_A_final as A  # noqa: E402
from calendar_preview import Canvas, F_CN  # noqa: E402
from PIL import ImageChops  # noqa: E402

OUT = Path("D:/zectrix/zectrix_yike/design")


def old_top_right(c: Canvas, lunar: str, fest: str | None) -> None:
    """**旧实现**（单节日时代），逐行照抄，仅供回归比对 —— 不要在生产代码里用它。"""
    if not fest:
        h = 21
        while h > 18 and c.text_width(lunar, F_CN, h) > A.BAR_W:
            h -= 1
        c.text(lunar, F_CN, h, A.RIGHT_X, 30, align="right")
        return

    h = 18
    while h > 15 and c.text_width(lunar, F_CN, h) > A.BAR_W:
        h -= 1
    c.text(lunar, F_CN, h, A.RIGHT_X, 17, align="right")
    tw = c.text_width(fest, F_CN, h)
    c.reversed_text(fest, F_CN, h, A.RIGHT_X - (tw + 20) // 2, 38, pad_x=10, pad_y=4)


def old_build(d: dt.date, shift: str, am: str, pm: str) -> tuple[Canvas, int]:
    import lunar_cn

    m = lunar_cn.day_info(d)
    m["shift"], m["am"], m["pm"] = shift or "", am or "", pm or ""
    term, fest = A.badges(d)
    c = Canvas()
    A.variant_a(c, m)
    name = fest[0] if fest else None
    old_top_right(c, m["lunar_cn"], name)
    A.draw_term_vertical(c, term)
    old_pt = c.font_for_height(name, F_CN, 18).size if name else 0
    return c, old_pt


CASES = [
    ("2026-09-25", "9DP1", "上午08:00-12:00", "下午14:00-18:00"),   # 单节日：中秋节
    ("2026-03-20", "9DP1", "上午08:00-12:00", "下午14:00-18:00"),   # 单节日 + 节气（旧实现踩中字号漂移）
    ("2038-02-04", "9DP1", "上午08:00-12:00", "下午14:00-18:00"),   # 春节（2 字，旧实现也漂）
    ("2026-09-23", "9DP1", "上午08:00-12:00", "下午14:00-18:00"),   # 无节日：只有农历
    ("2026-04-05", "9DP1", "上午08:00-12:00", "下午14:00-18:00"),   # 清明（去重路径）
]


def main() -> None:
    print("顶栏单签回归比对（旧实现 vs 当前实现）")
    print()
    for ds, shift, am, pm in CASES:
        d = dt.date.fromisoformat(ds)
        a, old_pt = old_build(d, shift, am, pm)
        b, term, fests, m = A.build(d, shift, am, pm)

        ba = a.img.point(lambda p: 255 if p > 127 else 0, "1")
        bb = b.img.point(lambda p: 255 if p > 127 else 0, "1")

        d_img = ImageChops.difference(ba.convert("L"), bb.convert("L"))
        bbox = d_img.getbbox()
        diff = sum(d_img.histogram()[1:])   # 非 0 灰度 = 有差异的像素数

        tag = f"{ds}  {' + '.join(fests) or '无节日'}"
        if diff == 0:
            print(f"  OK {tag:34s} 完全一致（0 像素差异）")
            continue

        why = (f"**字号归一化** pt{old_pt} → pt{A.FEST_PT}"
               if old_pt and old_pt != A.FEST_PT else "需人工确认")
        top_only = bbox[3] <= 52
        print(f"  !! {tag:34s} {diff:4d} 像素差异  bbox={bbox}  "
              f"{'顶栏内' if top_only else '**溢出顶栏**'}  ← {why}")

    print()
    print("判定口径：")
    print("  · 差异全部落在顶栏（y<=52）内，且原因标注为「字号归一化」→ 属**修正**。")
    print("    旧实现按墨迹高二分选字号，'龙抬头'(pt18→墨迹高19) / '春节' 会被误降到")
    print("    pt17 —— 同一屏上签的字号随节日名漂移。现在统一固定 pt18。")
    print("  · 若出现「溢出顶栏」或 0 差异以外的其他原因 → 必须人工核对。")


if __name__ == "__main__":
    main()
