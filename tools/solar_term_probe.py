"""节气可行性探针：把「通用寿星公式」和天文算法对撞，看误差到底多大。

目的不是选算法，而是回答一个问题：**节气做进固件，风险在哪、有多大**。

两条路线：
  A) 天文法 —— Meeus《Astronomical Algorithms》第 25 章低精度太阳视黄经，
     二分求「黄经 = 目标角」的时刻。精度约 0.01° ≈ 15 分钟，判日期足够。
     作为**基准真值**。
  B) 通用寿星公式 —— `日期 = [Y×D + C] - L`，D=0.2422，L=[Y/4]，
     每世纪每个节气一个常数 C，另有「例外年份」修正表。
     这是 C 端最省事的路子，也是各类日历程序普遍在用的。

输出：21 世纪逐年的差异，以及例外年份落在哪。
"""

from __future__ import annotations

import datetime as dt
import math

# 黄经目标角（度）
TERM_LON = {
    "清明": 15.0,
    "夏至": 90.0,
    "秋分": 180.0,
    "冬至": 270.0,
}

# 寿星公式：21 世纪 C 值（部分）
C_21 = {"清明": 4.81, "夏至": 21.37, "秋分": 23.042, "冬至": 21.94}
# 寿星公式：20 世纪 C 值
C_20 = {"清明": 5.59, "夏至": 22.20, "秋分": 23.822, "冬至": 22.60}

# 已知例外年份修正（21 世纪），值为天数偏移
EXC_21 = {
    "清明": {},
    "夏至": {1928: 1},
    "秋分": {1942: 1},
    "冬至": {2021: -1},
}

TZ_OFFSET_H = 8.0  # 北京时间


# ------------------------------------------------------------------ 天文法


def jd_from_utc(t: dt.datetime) -> float:
    """公历（UTC）→ 儒略日。"""
    y, m = t.year, t.month
    d = t.day + (t.hour + t.minute / 60 + t.second / 3600) / 24
    if m <= 2:
        y -= 1
        m += 12
    a = y // 100
    b = 2 - a + a // 4
    return int(365.25 * (y + 4716)) + int(30.6001 * (m + 1)) + d + b - 1524.5


def sun_apparent_lon(jd: float) -> float:
    """太阳视黄经（度），Meeus 低精度式。"""
    t = (jd - 2451545.0) / 36525.0
    l0 = 280.46646 + 36000.76983 * t + 0.0003032 * t * t
    m = math.radians(357.52911 + 35999.05029 * t - 0.0001537 * t * t)
    c = ((1.914602 - 0.004817 * t - 0.000014 * t * t) * math.sin(m)
         + (0.019993 - 0.000101 * t) * math.sin(2 * m)
         + 0.000289 * math.sin(3 * m))
    omega = math.radians(125.04 - 1934.136 * t)
    return (l0 + c - 0.00569 - 0.00478 * math.sin(omega)) % 360.0


def solve_term(year: int, term: str) -> dt.datetime:
    """求某年某节气的北京时间（天文法）。"""
    target = TERM_LON[term]
    # 各节气的搜索起点（月, 日）；区间给足
    guess = {"清明": (4, 1), "夏至": (6, 18), "秋分": (9, 20), "冬至": (12, 18)}[term]

    lo = dt.datetime(year, *guess, tzinfo=dt.timezone.utc)
    # 用「相对角度」单调递增来找穿越点
    def rel(t: dt.datetime) -> float:
        d = sun_apparent_lon(jd_from_utc(t)) - target
        while d < -180:
            d += 360
        while d > 180:
            d -= 360
        return d

    step = dt.timedelta(hours=6)
    hi = lo + step
    for _ in range(60):
        if rel(lo) <= 0 <= rel(hi):
            break
        lo, hi = hi, hi + step
    else:
        raise RuntimeError(f"{year} {term} 未找到穿越点")

    for _ in range(60):  # 二分到 1 秒内
        mid = lo + (hi - lo) / 2
        if rel(mid) < 0:
            lo = mid
        else:
            hi = mid
    return (lo + (hi - lo) / 2).astimezone(dt.timezone(dt.timedelta(hours=TZ_OFFSET_H)))


# ------------------------------------------------------------------ 寿星公式


def shouxing(year: int, term: str) -> int:
    """寿星公式算出的公历「日」。"""
    c = C_21[term] if year >= 2000 else C_20[term]
    y = year % 100
    day = int(y * 0.2422 + c) - y // 4
    day += EXC_21.get(term, {}).get(year, 0)
    return day


def main() -> None:
    print("黄经基准：", TERM_LON)
    print()
    for term in TERM_LON:
        print(f"===== {term} =====")
        bad: list[str] = []
        for year in range(2000, 2100):
            astro = solve_term(year, term)
            sx = shouxing(year, term)
            if astro.day != sx:
                bad.append(f"{year}(天文{astro.day}日/寿星{sx}日)")
        print(f"  2000–2099 共 100 年，寿星公式与天文法**不一致的年份：{len(bad)}**")
        if bad:
            print("   ", "  ".join(bad))
        print()

    print("===== 关键细节：临界时刻 =====")
    for year in (2021, 2022, 2023, 2024, 2025, 2026, 2027):
        for term in ("清明", "冬至"):
            t = solve_term(year, term)
            print(f"  {year} {term}: {t:%Y-%m-%d %H:%M}（北京时间）")


if __name__ == "__main__":
    main()
