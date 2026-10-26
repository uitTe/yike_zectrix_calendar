"""农历 / 节日显示层（台历页专用）。

数据来源：`lunardate`（纯 Python）。它内置的 `YEAR_INFOS` 就是公认的 lunarInfo 表
（1900–2099 共 200 项，开头 `0x4BD8 0x4AE0 0xA570 0x54D5 …`），
已用 2024 / 2025 / 2026 / 2027 / 2030 五个春节锚点校验**全部命中**。

**固件端要移植的正是同一张表 + 同一套解码**，编码规则：
    bit[3:0]  = 闰月月份（0 = 当年无闰月）
    bit[15:4] = 12 个月的月长，**bit15 = 正月 … bit4 = 腊月**
                （即 MSB→正月；1 = 30 天 / 0 = 29 天）
    若有闰月，则 bit[16] 为闰月的月长
基准：1900-01-31 为农历 1900 年正月初一。

⚠️ 2026-10-14 更正：旧注释写的「LSB→正月」**是错的**。核 `lunardate` 源码
   `_enum_month()` 取的是 `(year_info >> (16 - month))`，bit15 才是正月。
   照错的写法转 C 会让正月/腊月月长互换 → **农历整体错一天**。
   已按正确方向生成 `cal_lunar_table.c` 并通过 209,610 项黄金向量验证。

所以这里刻意只依赖这一个库、不引别的历法包 —— 转 C 时逐行对照即可。
"""

from __future__ import annotations

import datetime as dt

from lunardate import LunarDate

# ---------------------------------------------------------------- 中文名称表

LUNAR_MONTH_CN = ["正", "二", "三", "四", "五", "六", "七", "八", "九", "十", "冬", "腊"]

_D1 = "初一 初二 初三 初四 初五 初六 初七 初八 初九 初十".split()
_D2 = "十一 十二 十三 十四 十五 十六 十七 十八 十九".split()
_D3 = "二十 廿一 廿二 廿三 廿四 廿五 廿六 廿七 廿八 廿九".split()
_D4 = ["三十"]
LUNAR_DAY_CN = [""] + _D1 + _D2 + _D3 + _D4  # 下标 1..30

WEEKDAY_CN = ["一", "二", "三", "四", "五", "六", "日"]


# ---------------------------------------------------------------- 节日表
#
# 只收「一天定点」的节日（含某月第 N 个周几这类规则日）。
# 需要天文计算的节气节（清明 / 冬至）**暂不含**，见下方 TODO。

# 公历固定节日
FEST_SOLAR: dict[tuple[int, int], str] = {
    (1, 1): "元旦",
    (2, 14): "情人节",
    (3, 8): "妇女节",
    (3, 12): "植树节",
    (4, 1): "愚人节",
    (5, 1): "劳动节",
    (5, 4): "青年节",
    (6, 1): "儿童节",
    (7, 1): "建党节",
    (8, 1): "建军节",
    (9, 10): "教师节",
    (10, 1): "国庆节",
    (12, 24): "平安夜",
    (12, 25): "圣诞节",
}

# 农历固定节日（键 = 农历月, 农历日）
FEST_LUNAR: dict[tuple[int, int], str] = {
    (1, 1): "春节",
    (1, 15): "元宵节",
    (2, 2): "龙抬头",
    (5, 5): "端午节",
    (7, 7): "七夕节",
    (7, 15): "中元节",
    (8, 15): "中秋节",
    (9, 9): "重阳节",
    (12, 8): "腊八节",
    (12, 23): "小年",
}

# 优先级：数字越大越优先（同一天撞车时取最大的）。
# 传统大节 + 法定假日 > 纪念日 > 洋节。
FEST_PRIORITY = {
    "春节": 100, "中秋节": 95, "端午节": 90, "国庆节": 88, "元旦": 85,
    "元宵节": 80, "重阳节": 75, "七夕节": 70, "腊八节": 65, "小年": 60,
    "除夕": 100, "劳动节": 86, "儿童节": 70, "教师节": 68,
    "母亲节": 60, "父亲节": 60, "感恩节": 55,
}


def _nth_weekday(year: int, month: int, weekday: int, n: int) -> dt.date:
    """某年某月第 n 个星期 weekday（weekday: 0=周一 … 6=周日）。"""
    first = dt.date(year, month, 1)
    offset = (weekday - first.weekday()) % 7
    return first + dt.timedelta(days=offset + (n - 1) * 7)


# ---------------------------------------------------------------- 农历换算


def to_lunar(d: dt.date) -> LunarDate:
    """公历 → 农历。"""
    return LunarDate.from_solar_date(d.year, d.month, d.day)


def lunar_text(d: dt.date) -> str:
    """农历中文串，如 `八月十三`、`闰六月初一`、`腊月三十`。"""
    lu = to_lunar(d)
    prefix = "闰" if lu.isLeapMonth else ""
    return f"{prefix}{LUNAR_MONTH_CN[lu.month - 1]}月{LUNAR_DAY_CN[lu.day]}"


def is_lunar_new_year_eve(d: dt.date) -> bool:
    """今天是否为除夕（明天是正月初一）。

    这样判断比去翻年表算腊月有几天更稳，且天然覆盖 29/30 天的差异。
    """
    nxt = to_lunar(d + dt.timedelta(days=1))
    return nxt.month == 1 and nxt.day == 1 and not nxt.isLeapMonth


def festivals(d: dt.date) -> list[str]:
    """当天命中的**全部**节日名，按重要度降序；无节日返回 `[]`。

    同日撞车是真的会发生的（实测 1900–2099 共 **69 天**，全是 2 个、没有 3 个），
    典型如 `中秋节 + 国庆节`（2031-10-01）、`除夕 + 情人节`（2037-02-14）、
    `腊八节 + 元旦`（2031-01-01）—— 所以不能只留一个。

    排序按 `FEST_PRIORITY` 降序（= 重要度），同分时保持"除夕 → 农历节 → 公历节 →
    规则日"的探测顺序（Python 排序稳定）。最大的节日排最前，顶栏右对齐时它离屏边最近。
    """
    hits: list[str] = []

    if is_lunar_new_year_eve(d):
        hits.append("除夕")

    lu = to_lunar(d)
    if not lu.isLeapMonth:  # 闰月不过节
        name = FEST_LUNAR.get((lu.month, lu.day))
        if name:
            hits.append(name)

    name = FEST_SOLAR.get((d.month, d.day))
    if name:
        hits.append(name)

    # 规则日：母亲节 5 月第 2 个周日 / 父亲节 6 月第 3 个周日 / 感恩节 11 月第 4 个周四
    if d == _nth_weekday(d.year, 5, 6, 2):
        hits.append("母亲节")
    if d == _nth_weekday(d.year, 6, 6, 3):
        hits.append("父亲节")
    if d == _nth_weekday(d.year, 11, 3, 4):
        hits.append("感恩节")

    hits.sort(key=lambda n: -FEST_PRIORITY.get(n, 0))
    return hits


def festival(d: dt.date) -> str | None:
    """当天最重要（优先级最高）的那个节日名；无节日返回 None。

    只是 `festivals()` 的便捷包装，保持旧调用点可用。**展示层请用 `festivals()`**，
    否则同日撞车时会静默丢掉一个节日。
    """
    got = festivals(d)
    return got[0] if got else None


# TODO(节气)：清明与冬至属于**节气**，日期由太阳黄经决定，不是固定日，
# 不能用查表硬编码。需要 ① 通用寿星公式（24 节气 C 值表，仅 24 个整数）
# 或 ② 截断 VSOP87 天文算法。等大人确认要不要节气节再做。


# ---------------------------------------------------------------- 打包


def day_info(d: dt.date) -> dict:
    """台历页需要的全部日期信息。"""
    return {
        "date_cn": f"{d.year}年{d.month}月{d.day}日",
        "weekday_cn": "星期" + WEEKDAY_CN[d.weekday()],
        "lunar_cn": lunar_text(d),
        "festival": festival(d),        # 最重要那一个（兼容旧调用）
        "festivals": festivals(d),      # 全部（展示层用这个）
        "year": d.year, "month": d.month, "day": d.day,
    }


if __name__ == "__main__":
    import sys

    days = sys.argv[1:] or ["2026-09-23", "2026-09-25", "2027-02-05", "2027-02-06"]
    for s in days:
        d = dt.date.fromisoformat(s)
        info = day_info(d)
        print(f"{s}  {info['weekday_cn']}  农历 {info['lunar_cn']}"
              f"  节日 {info['festival'] or '—'}")
