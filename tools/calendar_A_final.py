"""A 版台历 · 定稿版式渲染器（唯一权威版本）。

`calendar_A_lunar.py` / `calendar_A_term.py` 是选型阶段的对比脚本，已经收敛到本文件。
**以后改版式只改这里**，改完重出图；固件字模按本文件的字号/坐标抽取。

布局（400x300 · 1bpp · 无灰阶）

      ┌────────────────────────────────────────────────────┐
      │ 2026年9月23日                      八月十三        │  顶栏  左：年/月/日 27px
      │                              〔国庆节〕〔中秋节〕   │        右：农历 18/21px
      ├──────────────────────────────┬─────────────────────┤        + 节日反白签（有才画，可多个并排）
      │                              │    星   │   秋      │
      │  上午08:00-12:00             │    期   │   分      │  主区  左：上午/下午 时段
      │  下午14:00-18:00             │         │           │        右：竖排星期 40px
      │                              │         │           │            + 竖排节气 30px
      ├──────────────────────────────┴─────────────────────┤
      │ 今日班次                              ▓早班▓        │  底栏  班次名反白
      └────────────────────────────────────────────────────┘

三条设计规则（都是从"墨水屏只有黑白"这个前提推出来的）

1. **层次只能靠「字号」和「反白」两档** —— 没有色彩可用，所以字号必须拉开级差：
   星期 40 > 节气 30 > 大日期 27 > 农历 21/18 > 节日反白（反差最高，压过字号）。
2. **节气竖排 = 星期右侧的独立一列，有则显示、无则不占位** ——
   星期列位置恒定（x=316），节气出现或消失都不会让已有元素移动一下。
3. **清明让位** —— 它是唯一「节气即节日」的（`清明` ⊂ `清明节`），
   否则同一天会在两处各写一遍。规则：节气名是节日名的子串时，不显示节气。

顶栏的节日签**可以不止一个**：同日撞车是真的会发生的（1900–2099 实测 69 天），
如 `2031-10-01 中秋节 + 国庆节`、`2037-02-14 除夕 + 情人节`。规则：
- 全部节日**并排右对齐**，各自一个独立反白签（语义上就是两件事，不合并成一个）；
- 整行右对齐 → 右边距恒定，只有左端随内容伸缩，视觉上不会左右乱窜；
- 签行左界按**实测标题宽度**动态算（不写死常量），塞不下就逐档降字号，最后才并签兜底。
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from calendar_preview import Canvas, F_CN, W  # noqa: E402
from calendar_preview import WEEK_STEP, WEEK_TOP, variant_a  # noqa: E402
import lunar_cn  # noqa: E402
import solar_terms  # noqa: E402

RIGHT_X = W - 16     # 顶栏内容右对齐基准
BAR_W = 156          # 顶栏右侧可用宽度（左侧标题 `2026年9月23日` 实测已占 200px）

# --- 顶栏节日反白签 ---------------------------------------------------------
# 反白签可以**不止一个** —— 同日撞车是真的（1900–2099 实测 69 天，见下方 badges()）。
# 实测几何：最宽标题 `2026年12月31日` pt27 墨迹 x ∈ [16, 209]，留 16px 间隙 → 左界 225；
# 右界 RIGHT_X=384 → **可用 159px**。
# 最坏是「两个 3 字节日并排」：pt18/pad10/gap14 要 162px —— 超了，于是自动退到
# pad9/gap13 = 157px 放下（**只缩内边距、不缩字号**，两个签看起来仍然一样大）。
# 单签时 74px 远小于预算，命中首档 → 外观与定稿逐像素一致。
FEST_PT = 18         # 签内字号基准（= 首档字号）
FEST_PT_MIN = 14     # 降级下限（1bpp 中文可读下限）
FEST_PAD_X = 10      # 签内左右留白（= 首档内边距）
FEST_BOX_H = 26      # 签高**固定** —— 不然 2 字签与 3 字签会一天胖一天瘦
FEST_GAP = 14        # 相邻两签的间隙（= 首档间隙）
FEST_TOP_GAP = 16    # 签行左界与标题墨迹之间的最小间隙
FEST_CY = 38         # 签的垂直中心（与单签时代的既有位置一致）
FEST_JOIN = "·"      # 兜底：全部降级档都塞不下时，多个节日并成**一个**签
BATTERY_PT = 18
BATTERY_CY = 70
WIFI_GAP = 10         # 信号格右边缘与电池左边缘的间距；低电量感叹号占电池左侧 7px，留 3px 透气
WIFI_BAR_W = 4        # 单根竖条宽
WIFI_BAR_GAP = 2      # 竖条之间的空隙
WIFI_BAR_H = (4, 7, 10, 13)   # 四根竖条的高度，底对齐
WIFI_TOTAL_W = WIFI_BAR_W * 4 + WIFI_BAR_GAP * 3   # 22
# (字号, 内边距, 间隙) —— 总宽超预算时逐档下探。**先缩内边距、后缩字号**：
# 反白签的"黑框"本身占视觉重量，缩 pad 比缩字号更不伤观感，且不会让同一天
# 的两个签大小不一。首档 = 定稿值，单签永远命中它。
_FEST_SCALES = (
    (FEST_PT, FEST_PAD_X, FEST_GAP),
    (18, 9, 13), (18, 8, 12),
    (17, 8, 12), (16, 7, 11), (15, 6, 10), (14, 6, 9),
)

# --- 节气列几何 -------------------------------------------------------------
# 实测：星期列中心 x=316，h40 单字墨迹宽 44 → 右边界 x=338，屏幕宽 400 → 剩 62px。
# 取列中心 367（h30 单字宽 31 → 墨迹 x∈[352,382]）：与星期留 14px、右边距 18px。
TERM_CX = 367
TERM_PT = 31         # 节气字号（**固定 pt**，同星期：逐字绘制不能用墨迹高二分）
TERM_STEP = 46       # 两字中心距。比星期的 52 略紧 —— 字小，用同样行距会显散
TERM_CY = WEEK_TOP + WEEK_STEP   # = 156，与星期块同中心，两列垂直方向对齐


def badges(d: dt.date) -> tuple[str | None, list[str]]:
    """当天的 (节气, 节日列表)，已做「清明」去重。无节日时列表为空。

    清明是唯一由节气驱动的法定节日（清明 ⊂ 清明节）——这种情况只保留节日，
    节气让位，避免同一件事在顶栏和右栏各显示一遍。

    **同日多个节日全部保留**（按重要度降序），由顶栏并排展示。
    实测 1900–2099 共 69 天撞车，全部是 2 个、没有 3 个；高频组合如
    `元宵节 + 情人节`(8×)、`中秋节 + 教师节`(8×)、`中秋节 + 国庆节`(6×)。
    """
    term = solar_terms.term_on(d)
    fests = lunar_cn.festivals(d)
    # 清明：节气本身没人过，过的是「清明节」。没登记就补一个。
    if term == "清明" and not any("清明" in f for f in fests):
        fests = ["清明节"]
    # 节气名被某个节日名包住 → 该节气让位（否则一天在两处各写一遍）
    if term and any(term in f for f in fests):
        term = None
    return term, fests


def _fest_row_width(c: Canvas, fests: list[str], pt: int, pad: int, gap: int) -> int:
    """一排反白签的总宽（含间隙）。"""
    return (sum(c.text_width_pt(t, F_CN, pt) + 2 * pad for t in fests)
            + gap * (len(fests) - 1))


def _pick_scale(c: Canvas, fests: list[str], avail: int) -> tuple[int, int, int] | None:
    """挑第一个能塞进 avail 的 (字号, 内边距, 间隙)；全都不行返回 None。"""
    for pt, pad, gap in _FEST_SCALES:
        if _fest_row_width(c, fests, pt, pad, gap) <= avail:
            return pt, pad, gap
    return None


def _draw_merged(c: Canvas, fests: list[str], avail: int) -> None:
    """兜底：全部降级档都塞不下 → 并成**一个**签（`中秋节·国庆节`）。

    首字仍是最高优先节日（`festivals()` 已排序），所以粗看语义不丢。
    实测 1900–2099 不会走到这里（最坏 `两个 3 字` 在基准档就放下了），
    留着是为了「以后往节日表里加长名字」时不至于画出屏。
    """
    pt, pad, _gap = _FEST_SCALES[-1]
    merged = FEST_JOIN.join(fests)
    total = c.text_width_pt(merged, F_CN, pt) + 2 * pad
    while pt > FEST_PT_MIN and total > avail:
        pt -= 1
        total = c.text_width_pt(merged, F_CN, pt) + 2 * pad
    while pad > 4 and total > avail:
        pad -= 1
        total = c.text_width_pt(merged, F_CN, pt) + 2 * pad
    c.reversed_box_pt(merged, F_CN, pt, RIGHT_X - total // 2, FEST_CY,
                      FEST_BOX_H, pad_x=pad)


def draw_top_right(c: Canvas, lunar: str, fests: list[str], left_limit: int) -> None:
    """顶栏右侧：农历一行；有节日时加第二行反白签（**同日多个就并排**）。

    无节日 → 农历单行居中于标题；有节日 → 农历上移一行，签行右对齐于 RIGHT_X。
    无论几个签，整行都**右对齐**，所以右侧留白恒定，只有左端随内容伸缩。
    """
    avail = RIGHT_X - left_limit

    if not fests:
        h = 21
        while h > 18 and c.text_width(lunar, F_CN, h) > avail:
            h -= 1
        c.text(lunar, F_CN, h, RIGHT_X, 30, align="right")
        return

    h = 18
    while h > 15 and c.text_width(lunar, F_CN, h) > avail:
        h -= 1
    c.text(lunar, F_CN, h, RIGHT_X, 17, align="right")

    chosen = _pick_scale(c, fests, avail)
    if chosen is None:
        _draw_merged(c, fests, avail)
        return

    pt, pad, gap = chosen
    x = RIGHT_X - _fest_row_width(c, fests, pt, pad, gap)
    for t in fests:
        bw = c.text_width_pt(t, F_CN, pt) + 2 * pad
        c.reversed_box_pt(t, F_CN, pt, x + bw // 2, FEST_CY, FEST_BOX_H, pad_x=pad)
        x += bw + gap


def draw_term_vertical(c: Canvas, term: str | None) -> None:
    """节气竖排于星期右侧。**有则显示，没有就整列不画**（星期位置不受影响）。"""
    if not term:
        return
    n = len(term)
    for i, ch in enumerate(term):
        y = TERM_CY + int((i - (n - 1) / 2) * TERM_STEP)
        c.text_pt(ch, F_CN, TERM_PT, TERM_CX, y)


def wifi_level(rssi: int) -> int:
    """RSSI 分档成 1～4 根竖条。已连接至少亮 1 根，不出现「连上了却没有格子」。"""
    if rssi >= -55:
        return 4
    if rssi >= -65:
        return 3
    if rssi >= -75:
        return 2
    return 1


def draw_wifi(c: Canvas, x_right: int, y_bottom: int, *, connected: bool,
              rssi: int = 0) -> None:
    """电量左侧的信号格。未连接画两条交叉斜线，表示断开。

    4px 宽的竖条画不了 2px 边框的空心（outline 宽度就是 2），所以「断开」不用空心，
    改用占满整个图标区的 ×，两种状态的形状差异最大，缩略图上也能分辨。
    """
    x0 = x_right - WIFI_TOTAL_W
    if connected:
        level = wifi_level(rssi)
        for i, bar_h in enumerate(WIFI_BAR_H):
            if i >= level:
                continue
            bx = x0 + i * (WIFI_BAR_W + WIFI_BAR_GAP)
            c.box(bx, y_bottom - bar_h, bx + WIFI_BAR_W, y_bottom)
        return
    span = WIFI_TOTAL_W - 1
    height = WIFI_BAR_H[-1] - 1
    for i in range(WIFI_TOTAL_W):
        down = y_bottom - 1 - i * height // span
        up = y_bottom - WIFI_BAR_H[-1] + i * height // span
        c.box(x0 + i, down, x0 + i + 1, down + 1)
        c.box(x0 + i, up, x0 + i + 1, up + 1)


def draw_battery(c: Canvas, percent: int, state: str = "normal", *,
                 wifi_rssi: int | None = None, wifi_connected: bool = False) -> None:
    """星期上方显示电池电量；输入来自设备或明确指定的预览参数。"""
    if not 0 <= percent <= 100 or state not in ("normal", "charging", "full", "absent", "fault"):
        raise ValueError("电量或电池状态无效")
    shown = 100 if state == "full" else percent // 5 * 5
    label = "--%" if state in ("absent", "fault") else f"{shown}%"
    width = c.text_width_pt(label, F_CN, BATTERY_PT)
    x, y = RIGHT_X - width - 28, BATTERY_CY - 6
    if wifi_rssi is not None:
        draw_wifi(c, x - WIFI_GAP, y + 12, connected=wifi_connected, rssi=wifi_rssi)
    c.box(x, y, x + 20, y + 12, outline=True)
    c.box(x + 20, y + 4, x + 22, y + 8)
    if state not in ("absent", "fault"):
        fill = 14 * shown // 100
        if fill:
            c.box(x + 3, y + 3, x + 3 + fill, y + 9)
    if state == "charging":
        bolt = ((10, 2), (9, 2), (8, 2), (7, 2), (6, 6), (9, 2), (8, 2), (7, 2))
        for row, (dx, size) in enumerate(bolt):
            c.d.rectangle((x + dx - 1, y + 1 + row, x + dx + size, y + 3 + row), fill=255)
        for row, (dx, size) in enumerate(bolt):
            c.box(x + dx, y + 2 + row, x + dx + size, y + 3 + row)
    elif state in ("absent", "fault") or (state == "normal" and percent < 20):
        c.box(x - 7, y + 1, x - 5, y + 8)
        c.box(x - 7, y + 10, x - 5, y + 12)
    c.text_pt(label, F_CN, BATTERY_PT, RIGHT_X, BATTERY_CY, align="right")


def build(d: dt.date, shift: str, am: str, pm: str, *,
          blank_schedule: bool = False, battery_percent: int | None = None,
          battery_state: str = "normal", wifi_rssi: int | None = None,
          wifi_connected: bool = False) -> tuple[Canvas, str | None, list[str], dict]:
    m = lunar_cn.day_info(d)
    # None / 缺失统一落成空串；空白、休息、获取失败三者的选择见 calendar_preview.variant_a。
    m["shift"], m["am"], m["pm"] = shift or "", am or "", pm or ""
    m["blank_schedule"] = blank_schedule
    term, fests = badges(d)
    c = Canvas()
    variant_a(c, m)                      # 骨架：日期 / 分隔线 / 星期 / 时段 / 底栏
    # 签行的左界由**实测的标题宽度**决定，而不是写死常量 —— 标题会随
    # `2026年9月23日` / `2026年12月31日` 长度微变，写死就会在某些月份压到标题。
    title_w = c.text_width_pt(m["date_cn"], F_CN, 27)
    draw_top_right(c, m["lunar_cn"], fests, 16 + title_w + FEST_TOP_GAP)
    draw_term_vertical(c, term)
    if battery_percent is not None:
        draw_battery(c, battery_percent, battery_state,
                     wifi_rssi=wifi_rssi, wifi_connected=wifi_connected)
    return c, term, fests, m


# ---------------------------------------------------------------- 数据契约
#
# 第三方数据源**只**提供这四项：
#     date（年月日） · shift（今日班次） · am（第 1 段时段） · pm（第 2 段时段）
# 其余全部由固件**离线算**：星期、农历、节气（天文法）、节日。
# 这正是既定架构原则的体现 ——「离线可算的放固件，行政/年度变动的走 API」。
#
# ⚠️ am / pm 的字段语义是「**第 1 段 / 第 2 段**」，不是真的上午下午 ——
#    夜班会给出 `20:00-24:00` / `00:00-04:00`。所以前缀**必须按开始时间判定**，
#    写死「上午/下午」就会出现「上午20:00-24:00」这种荒谬结果（实测确认过）。

# 按开始小时选前缀：<6 凌晨 / <12 上午 / <18 下午 / ≥18 夜间
_SPAN_PREFIXES = ("凌晨", "上午", "下午", "夜间")


def _start_hour(span: str) -> int | None:
    """从 `08:00-12:00` / `8:00-12:00` 里取开始小时；取不到返回 None。"""
    m = re.match(r"\s*(\d{1,2})\s*[:：]", span)
    if not m:
        return None
    h = int(m.group(1))
    return h if 0 <= h <= 23 else None


def with_prefix(span: str) -> str:
    """给时段补前缀。空值原样返回；数据源已自带前缀则不重复加。"""
    if not span:
        return ""
    if span.startswith(_SPAN_PREFIXES):
        return span
    h = _start_hour(span)
    if h is None:
        return span
    if h < 6:
        prefix = "凌晨"
    elif h < 12:
        prefix = "上午"
    elif h < 18:
        prefix = "下午"
    else:
        prefix = "夜间"
    return prefix + span


def load_payload(path: Path) -> list[tuple[dt.date, str, str, str]]:
    """读第三方 payload → [(日期, 班次名, 第1段时段, 第2段时段)]。

    字段缺失（该天没有排班，见 docs/03 §2.2）时落成空串，由调用方置
    `blank_schedule` 让主区留白。
    """
    data = json.loads(path.read_text(encoding="utf-8"))
    return [
        (dt.date.fromisoformat(it["date"]),
         it.get("shift", "") or "",
         with_prefix(it.get("am", "") or ""),
         with_prefix(it.get("pm", "") or ""))
        for it in data["days"]
    ]


def fest_geometry(c: Canvas, date_cn: str, fests: list[str]) -> str:
    """签行几何自检：实际总宽 / 可用预算 / 命中档位 / 余量。

    这行输出是**回归证据** —— 以后若改标题字号或签的 padding，从它就能立刻
    看出余量是不是被吃光了（最坏情形是两个 3 字节日 + 两位数的月/日）。
    """
    title_w = c.text_width_pt(date_cn, F_CN, 27)
    left = 16 + title_w + FEST_TOP_GAP
    avail = RIGHT_X - left
    got = _pick_scale(c, fests, avail)
    if got is None:
        return f"签行 **兜底并签** / 预算 {avail}px"
    pt, pad, gap = got
    row = _fest_row_width(c, fests, pt, pad, gap)
    return (f"签行 {row}px / 预算 {avail}px  余 {avail - row}px "
            f"(pt{pt} pad{pad} gap{gap})  "
            f"标题右界 x={16 + title_w} → 签左界 x={RIGHT_X - row}")


def main() -> None:
    ap = argparse.ArgumentParser(description="A 版台历定稿版式渲染")
    ap.add_argument("--out", default="D:/zectrix/zectrix_yike/design")
    ap.add_argument("--json", type=Path, default=None,
                    help="第三方 payload 文件；给了就按数据契约驱动渲染")
    ap.add_argument("--dates", nargs="*", default=[
        "2026-09-23",   # 秋分 —— 有节气、无节日（今天）
        "2026-09-24",   # 农历八月十四 —— 无节气、无节日
        "2026-09-25",   # 中秋节 —— 无节气、有节日
        "2026-03-20",   # 春分 = 龙抬头 —— 两者都有（今年就撞上了）
        "2038-02-04",   # 立春 = 春节 —— 撞得最狠的组合
        "2026-04-05",   # 清明 —— 节气即节日，验证去重
        "2031-10-01",   # 中秋节 + 国庆节 —— **两个 3 字节日**，签行最宽的情形
        "2031-01-01",   # 腊八节 + 元旦 —— 3 字 + 2 字混合
        "2037-02-14",   # 除夕 + 情人节 —— 农历侧 2 字 + 公历侧 3 字
    ])
    ap.add_argument("--shift", default="9DP1")
    ap.add_argument("--am", default="上午08:00-12:00")
    ap.add_argument("--pm", default="下午14:00-18:00")
    ap.add_argument("--blank-schedule", action="store_true", help="班次和时段保持空白")
    ap.add_argument("--battery-percent", type=int, help="明确指定预览示例电量，范围 0～100")
    ap.add_argument("--battery-state", choices=("normal", "charging", "full", "absent", "fault"), default="normal")
    ap.add_argument("--wifi-rssi", type=int, default=None,
                    help="已连接时的信号强度；配合 --wifi-connected 使用，不给则不画信号格")
    ap.add_argument("--wifi-connected", action="store_true",
                    help="已连接；不给时画断开的交叉斜线")
    args = ap.parse_args()

    if args.json:
        items = load_payload(args.json)
    else:
        items = [(dt.date.fromisoformat(s), args.shift,
                  with_prefix(args.am), with_prefix(args.pm))
                 for s in args.dates]

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    for d, shift, am, pm in items:
        c, term, fests, m = build(d, shift, am, pm, blank_schedule=args.blank_schedule,
                                 battery_percent=args.battery_percent, battery_state=args.battery_state,
                                 wifi_rssi=args.wifi_rssi, wifi_connected=args.wifi_connected)
        p = out / f"calendar_A_final_{d.isoformat()}.png"
        c.save_1bpp(p)
        fest_s = " + ".join(fests) or "—"
        n_tag = f" ({len(fests)}签)" if len(fests) > 1 else ""
        print(f"{d} {m['weekday_cn']}  农历 {m['lunar_cn']:6s} "
              f"节气 {term or '—':4s} 节日 {fest_s}{n_tag} 班次 {shift}")
        print(f"    -> {p.name}")
        if fests:
            print("    " + fest_geometry(c, m["date_cn"], fests))


if __name__ == "__main__":
    main()
