"""节气 × 节日 同一天时的展示策略对比。

背景：扫描 2024–2060 共 37 年，**每年平均约 1 例**节气与节日撞车（共 38 例），
不是边角情况。撞得最狠的是 2038-02-04「立春 + 春节」、2048-09-22「秋分 + 中秋节」。
另有 2026-03-20「春分 + 龙抬头」这类。

所以必须定一条明确规则，否则同一天会出现两个"今天是什么日子"的标签互相打架。

三种策略：
  T1 合并签   —— 反白签内容 = `节气 · 节日`，两者**平级**同处一签
  T2 分组两行 —— 行1 `农历 · 节气`（纯文字），行2 节日反白签；**分级**（节气不反白）
  T3 节日覆盖 —— 节日优先，有节日则节气**不显示**；只有节气时才把节气做成反白签

另：**清明是唯一「节气即节日」**的，会自然去重（`清明` 是 `清明节` 的子串 → 只留节日）。
"""

from __future__ import annotations

import argparse
import datetime as dt
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from calendar_preview import Canvas, F_CN, W  # noqa: E402
from calendar_preview import variant_a  # noqa: E402
import lunar_cn  # noqa: E402
import solar_terms  # noqa: E402

RIGHT_X = W - 16
BAR_W = 156          # 顶栏右侧实测预算


def term_of(d: dt.date) -> str | None:
    """当天节气名；无则 None。"""
    for name, date in solar_terms.terms_of_year(d.year).items():
        if date == d:
            return name
    return None


def badges(d: dt.date) -> tuple[str | None, str | None]:
    """当天的 (节气, 节日)，已做「清明」去重。

    清明既是节气又是节日（且是法定假日）——`清明` 是 `清明节` 的子串，
    这种情况只保留节日，节气让位，避免同一件事显示两遍。
    """
    term = term_of(d)
    fest = lunar_cn.festival(d)
    # 节气驱动的节日：清明
    if term == "清明" and not fest:
        fest = "清明节"
    if term and fest and term in fest:
        term = None
    return term, fest


# ------------------------------------------------------------------ 三种策略


def fit_h(c: Canvas, text: str, max_w: int, h_top: int = 21, h_min: int = 14) -> int:
    """把字号收缩到文本宽度不超过 max_w。

    ⚠️ 别省这一步：`八月十三 · 秋分` 在 h21 下约 190px，
    而顶栏右侧预算只有 156px —— 不收缩就会顶到左边标题上。
    """
    h = h_top
    while h > h_min and c.text_width(text, F_CN, h) > max_w:
        h -= 1
    return h


def draw_t1(c: Canvas, lunar: str, term: str | None, fest: str | None) -> None:
    """合并签：`节气 · 节日` 都在反白签里。"""
    label = " · ".join(x for x in (term, fest) if x)
    if not label:
        c.text(lunar, F_CN, fit_h(c, lunar, BAR_W), RIGHT_X, 30, align="right")
        return
    h = fit_h(c, label, BAR_W - 20, h_top=18)
    c.text(lunar, F_CN, min(h, fit_h(c, lunar, BAR_W)), RIGHT_X, 17, align="right")
    tw = c.text_width(label, F_CN, h)
    c.reversed_text(label, F_CN, h, RIGHT_X - (tw + 20) // 2, 38,
                    pad_x=10, pad_y=4)


def draw_t2(c: Canvas, lunar: str, term: str | None, fest: str | None) -> None:
    """分组两行：行1 `农历 · 节气`（纯文字），行2 节日反白签。

    层次：大日期(27) > 农历·节气(18) > 节日反白（反差最高）。
    有节气时行1 限高 18 —— 不然 `八月十三 · 秋分` 会顶到 h21（141px），
    跟左边大日期同级，看着像一行连着的长句。
    """
    line1 = lunar + (f" · {term}" if term else "")
    if not fest:
        h = fit_h(c, line1, BAR_W, h_top=18 if term else 21)
        c.text(line1, F_CN, h, RIGHT_X, 17 if term else 30, align="right")
        return
    h = fit_h(c, line1, BAR_W, h_top=18)
    c.text(line1, F_CN, h, RIGHT_X, 17, align="right")
    tw = c.text_width(fest, F_CN, h)
    c.reversed_text(fest, F_CN, h, RIGHT_X - (tw + 20) // 2, 38,
                    pad_x=10, pad_y=4)


def draw_t3(c: Canvas, lunar: str, term: str | None, fest: str | None) -> None:
    """节日覆盖：只标一件，节日优先；无节日才用节气。"""
    shown = fest or term
    if not shown:
        c.text(lunar, F_CN, fit_h(c, lunar, BAR_W), RIGHT_X, 30, align="right")
        return
    h = fit_h(c, shown, BAR_W - 20, h_top=18)
    c.text(lunar, F_CN, min(h, fit_h(c, lunar, BAR_W)), RIGHT_X, 17, align="right")
    tw = c.text_width(shown, F_CN, h)
    c.reversed_text(shown, F_CN, h, RIGHT_X - (tw + 20) // 2, 38,
                    pad_x=10, pad_y=4)


MODES = {
    "T1": ("合并签 · 节气与节日平级同签", draw_t1),
    "T2": ("分组两行 · 节气纯文字 / 节日反白", draw_t2),
    "T3": ("节日覆盖 · 只标一件，节日优先", draw_t3),
}


def main() -> None:
    ap = argparse.ArgumentParser(description="节气×节日的展示策略对比")
    ap.add_argument("--out", default="D:/zectrix/zectrix_yike/design")
    ap.add_argument("--dates", nargs="*", default=[
        "2026-09-23",   # 秋分（节气单独，今天）
        "2026-09-25",   # 中秋节（节日单独）
        "2026-03-20",   # 春分 = 龙抬头（撞车）
        "2038-02-04",   # 立春 = 春节（两个都重要的极端案例）
    ])
    ap.add_argument("--shift", default="早班")
    ap.add_argument("--am", default="上午08:00-12:00")
    ap.add_argument("--pm", default="下午14:00-18:00")
    args = ap.parse_args()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    for s in args.dates:
        d = dt.date.fromisoformat(s)
        term, fest = badges(d)
        m = lunar_cn.day_info(d)
        m["shift"], m["am"], m["pm"] = args.shift, args.am, args.pm
        print(f"\n{s} {m['weekday_cn']}  农历 {m['lunar_cn']}  "
              f"节气 {term or '—'}  节日 {fest or '—'}")
        for key, (desc, fn) in MODES.items():
            c = Canvas()
            variant_a(c, m)
            fn(c, m["lunar_cn"], term, fest)
            p = out / f"calendar_A_term_{key}_{s}.png"
            c.save_1bpp(p)
            print(f"    {key} {desc}")
            print(f"       -> {p.name}")

    print("\n平级/分级/覆盖三种取向的直观差异见 @2x 图。")


if __name__ == "__main__":
    main()
