"""台历页点阵字库清单（**人读版**）+ 三个字符集构造器。

⚠️ 清单的**真源是 `tools/pt_census.py` 的自动普查**（落盘 `tools/testdata/font_charset.json`），
**不要在本文件手写 pt 清单**。原因：渲染器的 `Canvas.font_for_height()` 按墨迹高
二分选字号，同一区域会随内容落在一批不同 pt 上（实测「主区时段」跨 pt17–38 共 20 档、
农历跨 pt15–21 共 9 档）。本文件的早期版本手写了 5 个 pt，实测需要 **25 个** —— 严重漏项。

本文件保留的职责：

* `_lunar_chars()` / `_festival_chars()` / `_term_chars()`
  —— 三个**字符集构造器**，被 `pt_census.py` 用来给出固定 pt 区域的字符集；
* `main()` —— 读普查清单，打一份带用途注释的人读版。

用法：python tools/charset_plan.py
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import lunar_cn  # noqa: E402
import solar_terms  # noqa: E402

PLAN = Path(__file__).resolve().parent / "testdata" / "font_charset.json"

DIGITS = set("0123456789")
LETTERS = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ")
PUNCT = set("()-:")


def _lunar_chars() -> set[str]:
    """农历用字：12 个月名 + 30 个日名 + 「月」「闰」。"""
    return (set("".join(lunar_cn.LUNAR_MONTH_CN)) | set("月")
            | set("".join(lunar_cn.LUNAR_DAY_CN)) | set("闰"))


def _festival_chars() -> set[str]:
    """全部节日名用字（含清明、除夕这类特殊项）+ 并签兜底分隔符。

    ⚠️ **同日多个节日会并排展示**（1900–2099 有 69 天撞车，如 `中秋节 + 国庆节`），
    但它们用的是**同一张节日表**里的字，不新增任何汉字 —— 这一组已经是全覆盖。
    只有 `·`（`FEST_JOIN`，极端情况下把多个节日并成一个签时用）需要额外备一份。
    """
    names = (list(lunar_cn.FEST_SOLAR.values())
             + list(lunar_cn.FEST_LUNAR.values())
             + ["除夕", "母亲节", "父亲节", "感恩节", "清明节"])
    return set("".join(names)) | {"·"}


def _term_chars() -> set[str]:
    """24 节气名去重用字。"""
    return set("".join(name for name, _, _ in solar_terms.TERMS))


def main() -> None:
    if not PLAN.exists():
        raise SystemExit(f"清单不存在，先跑：python tools/pt_census.py\n（缺少 {PLAN}）")
    plan = json.loads(PLAN.read_text(encoding="utf-8"))
    groups = plan["groups"]

    print("=" * 78)
    print(f"台历页点阵字库清单 · 普查内容域 {plan['content_domain']} · "
          f"{len(groups)} 个字号组")
    print("=" * 78)
    print(f"{'pt':>4} {'字数':>5} {'点阵':>9} {'索引':>8}  区域")
    print("-" * 78)
    total = index = 0
    for g in groups:
        pt, n = g["pt"], len(g["chars"])
        raw = ((pt * pt + 7) // 8) * n
        idx = n * 14
        total += raw
        index += idx
        print(f"{pt:>4} {n:>5} {raw / 1024:>7.1f}K {idx / 1024:>6.1f}K  "
              f"{' + '.join(g['regions'])}")
    print("-" * 78)
    print(f"点阵 {total / 1024:.1f} KB ｜ 索引 {index / 1024:.1f} KB ｜ "
          f"合计 {(total + index) / 1024:.1f} KB"
          "（相对 app 分区的 ~6 MB 可忽略）")
    print()
    print("--- 逐组字符清单 ---")
    for g in groups:
        print(f"\n[pt{g['pt']} · {' + '.join(g['regions'])}] {len(g['chars'])} 字")
        print("  " + g["chars"])


if __name__ == "__main__":
    main()
