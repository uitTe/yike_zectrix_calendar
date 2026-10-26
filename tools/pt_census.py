"""台历页「字号清单」唯一真源（普查 + 固定 pt 合并）。

为什么需要这一步
----------------
渲染器里有**两类**选字号路径，固件端必须分别对待：

1. **固定 pt**（逐字绘制 / 反白块）—— pt 由代码常量直接决定，不随内容漂移，
   但有两处是「逐 1 降级」的**连续区间**（底栏班次名 28→20、节日签 18→14）。
2. **按墨迹高**（`Canvas.font_for_height`）—— 二分搜索「使整串墨迹高 ≤ target_h
   的最大 pt」，结果**随内容与 target_h 一起变**。

固件端没有字体度量、只有固定字号点阵表 ⇒ 必须先穷举 1900–2099 的**全部真实内容**，
把两类路径实际会出现的 pt 集合定死，否则运行期会画出空白。

⚠️ 固件端**不能复刻那个二分**：搜索区间是 6..400，二分路径上会访问
41 / 54 / 104 / 203 这类大 pt —— 单是 pt203 的中文点阵每字就要 5 KB（33 字 ≈ 170 KB）。
正确做法是把本脚本算出的 **pt 候选集**固化成字库，固件端在候选集内
「**取最大满足 pt，且该组能完整画出整串**」（`cal_font_pick_pt()`）。

⚠️ **两条判据缺一不可**（2026-10-14 板端自检抓出的真 bug，详见 docs/03 §4.1）：

  · 必须取**最大**，不能「沿组顺序取第一个满足的」—— 字库组的排列顺序不可依赖
    （实测是 pt **升序**，取第一个满足的会退化成**最小** pt，整页字号全线塌到 pt14）。
  · 必须要求该组 `missing == 0` —— 本脚本的等价性验证假设「候选集内每组都能画全
    该串」，但固件侧每组字符集不同：混合内容（如顶栏的「2026年9月23日」）在只含
    时段字符的组里会缺字，而缺字不进包围盒 → 高度**虚小** → 误命中。

本脚本的 `--verify` 证明的是「候选集查找 vs 二分」的**数学等价**（不含缺字情形），
只是**必要条件**；完整验证在 `tools/font_missing_test.cc` 用例 ⑤ 与板端同源 10 例。

去重技巧（关键，让穷举不出分钟级）
----------------------------------
Pillow `textbbox` 的墨迹高 = 串内所有字形墨迹的**并集**高度，
**只依赖字符集，与字符顺序、重复次数无关**。故穷举时按 `frozenset(text)` 签名去重：
同一签名的所有文本墨迹高必然相同 ⇒ pt 必然相同 ⇒ 数学上完全等价，
而 74 400 条日期串会塌缩成几十个签名。

输出
----
`tools/testdata/font_charset.json` —— `tools/font_bitmap.py` 据此生成点阵字库。

用法：python tools/pt_census.py
"""

from __future__ import annotations

import json
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import calendar_A_final as caf  # noqa: E402
import calendar_preview as cp  # noqa: E402
import charset_plan  # noqa: E402
import lunar_cn  # noqa: E402
from calendar_preview import FAIL_TEXT, F_CN, REST_PT, WEEK_PT, Canvas  # noqa: E402

OUT = Path(__file__).resolve().parent / "testdata" / "font_charset.json"

FIRST_YEAR, LAST_YEAR = 1900, 2099
_MDAYS = (31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31)


def _is_leap(y: int) -> bool:
    return y % 4 == 0 and (y % 100 != 0 or y % 400 == 0)


# ------------------------------------------------------------------ 内容域

def date_texts() -> set[str]:
    """顶栏年月日：`YYYY年M月D日`（**真值，不补零**）。"""
    out: set[str] = set()
    for y in range(FIRST_YEAR, LAST_YEAR + 1):
        for m in range(1, 13):
            dim = _MDAYS[m - 1] + (1 if m == 2 and _is_leap(y) else 0)
            for d in range(1, dim + 1):
                out.add(f"{y}年{m}月{d}日")
    return out


def lunar_texts() -> set[str]:
    """顶栏农历：`月名+日名`，闰月带「闰」前缀。12 月名 × 30 日名 × {非闰, 闰}。"""
    return {f"{pre}{m}月{d}"
            for pre in ("", "闰")
            for m in lunar_cn.LUNAR_MONTH_CN
            for d in lunar_cn.LUNAR_DAY_CN[1:]}


def span_texts() -> set[str]:
    """主区时段：`前缀 + H:MM-H:MM`。

    前缀是 `with_prefix()` 按**开始小时**补的（<6 凌晨 / <12 上午 / <18 下午 / ≥18 夜间），
    另外把「数据源已自带前缀 → 原样返回」这条分支也算进来。
    小时可能是 1 位或 2 位、末段可达 `24:00`，故样本要覆盖全部数字字形与全部长度形态。
    """
    core: set[str] = set()
    for a in ("0", "00", "6", "06", "8", "08", "9", "12", "18", "20", "23"):
        for b in ("0", "00", "4", "04", "6", "12", "18", "22", "24"):
            core.add(f"{a}:00-{b}:00")
            core.add(f"{a}:30-{b}:45")
    core |= {"20:00-24:00", "00:00-04:00", "6:00-6:30", "08:00-12:00"}
    return {p + t for p in ("", "凌晨", "上午", "下午", "夜间") for t in core}


def _by_signature(texts: set[str]) -> dict[frozenset, str]:
    """按 `frozenset(text)` 去重（墨迹高只依赖字符集，见模块 docstring）。"""
    sig: dict[frozenset, str] = {}
    for t in sorted(texts):
        sig.setdefault(frozenset(t), t)
    return sig


# ------------------------------------------------------------------ 普查

def scan_ink_height(c: Canvas, texts: set[str], hs: list[int]
                    ) -> tuple[dict[int, set[str]], int, int]:
    """跑「按墨迹高选字号」路径，返回 (pt → 字符集, 签名数, 命中组合数)。"""
    sigs = _by_signature(texts)
    per_pt: dict[int, set[str]] = defaultdict(set)
    hits = 0
    for rep in sigs.values():
        for h in hs:
            per_pt[c.font_for_height(rep, F_CN, h).size] |= set(rep)
            hits += 1
    return per_pt, len(sigs), hits


# 班次名括号内的中文：**第三方数据源自定义、不可穷举** ⇒ 只能按实测形态逐个累积。
# 每个新值必须在这里显式登记，否则该字在屏幕上会**画成空白**（点阵字库查不到字形）。
_SHIFT_PAREN_CN = (
    "绿",      # `9DP(绿)` / `9DP1(绿)`
    "验车",    # `9DP0(验车)`
    "值班",    # `9DP1(值班)` —— 2026-10-14 大人补充
)

# 休息日的底栏**仍会画班次名**（只有主区换成「休息」大字），故必须能画出「休」「息」。
_SHIFT_REST_CN = "休息"

# 班次名的**已知形态**（含括号内备注）—— 字模清单与测试用例共用的唯一真源。
# `tools/font_verify.py` 的词汇守卫就遍历这里：每个形态的每个字，必须在 pt20–28
# 每一档都在字库内。新增形态时**只需改这一处**，两个脚本自动跟上。
SHIFT_KNOWN_FORMS: tuple[str, ...] = (
    "休", "休息",
    "9DP1", "9NP1",
    "9DP(绿)", "9DP1(绿)", "9NP1(绿)",
    "9DP0(验车)",
    "9DP1(值班)", "9DP0(值班)",
)


def shift_charset() -> set[str]:
    """底栏班次名字符集（实测形态的保守超集）。

    `normalize_shift()` 先把全角折成半角，所以这里只需半角 ASCII。
    """
    return (set("ABCDEFGHIJKLMNOPQRSTUVWXYZ") | set("abcdefghijklmnopqrstuvwxyz")
            | set("0123456789") | set("()-:")
            | set("".join(_SHIFT_PAREN_CN)) | set(_SHIFT_REST_CN))


def check_shift_vocabulary(verbose: bool = True) -> list[str]:
    """双向一致性检查 —— 防「形态加了字没进清单」与「清单里的字没人用」。

    单向检查救不了 2026-10-14 那次事故：新词 `值班` 只在数据源里出现，
    清单和测试用例都没提，于是谁都不报错、屏幕上直接画成空白。
    """
    problems: list[str] = []
    cs = shift_charset()

    used = set("".join(SHIFT_KNOWN_FORMS))
    for ch in sorted(used - cs):
        problems.append(f"已知形态用到「{ch}」但不在字符集内（先加进 _SHIFT_PAREN_CN）")

    declared = set("".join(_SHIFT_PAREN_CN)) | set(_SHIFT_REST_CN)
    for ch in sorted(declared - used):
        problems.append(f"字符集里的「{ch}」没有任何已知形态用到（形态表漏了一条）")

    if verbose:
        print(f"  已知形态 {len(SHIFT_KNOWN_FORMS)} 种 → 用字 "
              f"{len(used)} 个；字符集 {len(cs)} 字")
        for p in problems:
            print(f"    ❌ {p}")
        if not problems:
            print("    ✅ 形态表 ↔ 字符集 双向一致")
    return problems


def scan_literal_pt(texts: set[str], pts: list[int]) -> dict[int, set[str]]:
    """按**字面 pt** 收集字符集 —— 用于「量宽」区域。

    ⚠️ 不能复用 `scan_ink_height()`：那个会先过 `font_for_height()` 把 target_h 折算成
    实际 pt（量宽 h=27 → pt28），而测量调用用的是**字面 pt**（`text_width_pt(t, 27)`）。
    两者混起来就会出现「pt27 组永远收不到日期字符」，而渲染器偏偏要量 pt27。
    """
    per_pt: dict[int, set[str]] = defaultdict(set)
    for rep in _by_signature(texts).values():
        for pt in pts:
            per_pt[pt] |= set(rep)
    return per_pt


def fixed_regions() -> list[tuple[int, str, set[str]]]:
    """固定 pt 的区域（含逐 1 降级的连续区间）。"""
    out: list[tuple[int, str, set[str]]] = [
        (caf.BATTERY_PT, "固定 · 电量", set("0123456789%-")),
        (REST_PT, "固定 · 主区休息日「休息」", set("休息")),
        (WEEK_PT, "固定 · 竖排星期", set("星期一二三四五六日")),
        (caf.TERM_PT, "固定 · 竖排节气", charset_plan._term_chars()),
    ]
    shift = shift_charset()
    for pt in range(cp.SHIFT_PT_MIN, cp.SHIFT_PT + 1):
        out.append((pt, "固定 · 底栏班次名", shift))
    fest = charset_plan._festival_chars() | charset_plan._lunar_chars()
    for pt in range(caf.FEST_PT_MIN, caf.FEST_PT + 1):
        out.append((pt, "固定 · 顶栏节日反白签", fest))
    return out


# ------------------------------------------------------------------ 主流程

INK_REGIONS: list[tuple[str, set[str], list[int]]] = [
    ("顶栏 年月日",           date_texts(),  [27]),
    ("底栏 标签「今日班次」",  {"今日班次"},   [23]),
    ("主区 「获取失败」",      {FAIL_TEXT},   list(range(18, 31))),
    ("主区 时段",             span_texts(),  list(range(18, 31))),
    ("顶栏 农历（无节日）",    lunar_texts(), list(range(18, 22))),
    ("顶栏 农历（有节日）",    lunar_texts(), list(range(15, 19))),
]

# 「量宽」区域 —— 渲染器**测量**文本宽度（不是画）时用到的 (pt, 字符集)。
#
# 为什么必须单独登记：`calendar_A_final.build()` 用
# `text_width_pt(date_cn, **27**)` 量标题宽度来定节日签行的左界，而标题本身是按
# 墨迹高 27 选中 **pt28** 画出来的 —— **量的 pt 与画的 pt 不是同一个**。
#
# 2026-10-14 移植渲染器到 C 时踩到：pt27 组里当时只有数字（普查只记了「绘制」用到的
# pt 与字符），年月日缺失 → `cal_text_width_pt(27, date_cn)` 把缺字排除在包围盒外、
# 量出的宽度虚小 → 签行预算 avail 变大 → 最坏情形 2031-10-01（中秋 + 国庆，两个
# 3 字节日）少降一档，签行比定稿宽 5px。宿主逐像素比对报了 880 px 差异。
#
# 收缩循环里对农历 / 「获取失败」/ 时段逐档量宽用的 pt，都落在各自墨迹高区域已经
# 覆盖的集合内（量宽的 h 域 ⊆ 普查的 hs 域），故无需重复登记。
MEASURE_REGIONS: list[tuple[str, set[str], list[int]]] = [
    ("顶栏 年月日（量宽）", date_texts(), [27]),
]


def build_plan(verbose: bool = True) -> dict:
    c = Canvas()
    per_pt: dict[int, set[str]] = defaultdict(set)
    sources: dict[int, list[str]] = defaultdict(list)
    region_pts: dict[str, list[int]] = {}

    if verbose:
        print("=" * 76)
        print("台历页字号普查 · 内容域 1900–2099 真实值 · 按字符集签名去重")
        print("=" * 76)
        print("\n【一】按墨迹高选字号的区域（`Canvas.font_for_height`）\n")

    for name, texts, hs in INK_REGIONS:
        got, n_sig, n_hit = scan_ink_height(c, texts, hs)
        region_pts[name] = sorted(got)
        for pt, chars in got.items():
            per_pt[pt] |= chars
            if name not in sources[pt]:
                sources[pt].append(name)
        if verbose:
            print(f"  {name}")
            print(f"    内容域 {len(texts):>6,} 串 → 签名去重 {n_sig:>3} 个 "
                  f"× target_h {len(hs)} 档 = {n_hit} 次\n"
                  f"    → pt 集合 {sorted(got)}")

    if verbose:
        print("\n【一·补】按「量宽」用到的 pt（测量 ≠ 绘制，见 MEASURE_REGIONS 注释）\n")

    for name, texts, pts in MEASURE_REGIONS:
        got = scan_literal_pt(texts, pts)
        region_pts[name] = sorted(got)
        for pt, chars in got.items():
            per_pt[pt] |= chars
            if name not in sources[pt]:
                sources[pt].append(name)
        if verbose:
            print(f"  {name}")
            print(f"    内容域 {len(texts):>6,} 串 → 签名去重 {len(_by_signature(texts)):>3} 个 "
                  f"× 字面 pt {len(pts)} 档\n"
                  f"    → pt 集合 {sorted(got)}")

    if verbose:
        print("\n【二】固定 pt 的区域（常量决定；两处为逐 1 降级区间）\n")

    seen_fixed: dict[str, list[int]] = defaultdict(list)
    for pt, name, chars in fixed_regions():
        per_pt[pt] |= chars
        if name not in sources[pt]:
            sources[pt].append(name)
        seen_fixed[name].append(pt)
    if verbose:
        for name, pts in seen_fixed.items():
            rng = f"pt{min(pts)}" if len(pts) == 1 else f"pt{min(pts)}–{max(pts)}"
            print(f"  {name:<26s} {rng}")

    # ---------------------------------------------------------------- 汇总
    pts = sorted(per_pt)
    total_bitmap = 0
    groups = []
    for pt in pts:
        chars = sorted(per_pt[pt], key=ord)
        per = (pt * pt + 7) // 8
        vol = len(chars) * per
        total_bitmap += vol
        groups.append({"pt": pt, "chars": "".join(chars),
                       "regions": sorted(sources[pt])})

    index_bytes = sum(len(g["chars"]) * 14 for g in groups)

    if verbose:
        print()
        print("=" * 76)
        print(f"汇总：字库必须覆盖 {len(pts)} 个字号组")
        print("=" * 76)
        print(f"{'pt':>4} {'字数':>5} {'点阵':>9} {'索引':>8}  区域")
        for g in groups:
            pt, n = g["pt"], len(g["chars"])
            raw = (pt * pt + 7) // 8
            print(f"{pt:>4} {n:>5} {raw * n / 1024:>7.1f}K {n * 14 / 1024:>6.1f}K  "
                  f"{' + '.join(g['regions'])}")
        print("-" * 76)
        print(f"点阵合计 {total_bitmap / 1024:.1f} KB ｜ 索引约 {index_bytes / 1024:.1f} KB "
              f"｜ 合计约 {(total_bitmap + index_bytes) / 1024:.1f} KB")
        print("（相对 app 分区的 ~6 MB 可忽略）")

    # ---------------------------------------------------------------- 落盘
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps({
        "generated_by": "tools/pt_census.py",
        "content_domain": f"{FIRST_YEAR}-{LAST_YEAR}",
        "note": ("固件端在候选集内取「能完整画出整串（missing==0）、且高度 ≤ target_h」"
                 "的 **最大 pt**（= cal_font_pick_pt()）。本文件的 --verify 只证明"
                 "「候选集查找 vs 二分」的数学等价，不含缺字情形；缺字判据见 docs/03 §4.1。"),
        "candidate_pt": pts,
        "regions": {
            name: {"target_h": hs, "pt": region_pts[name]}
            for name, _texts, hs in (INK_REGIONS + MEASURE_REGIONS)
        },
        "fixed_pt": {name: sorted(s) for name, s in seen_fixed.items()},
        "groups": groups,
    }, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if verbose:
        print(f"\n写出 {OUT.relative_to(OUT.parent.parent.parent)}")

    return {"groups": groups, "total_bitmap": total_bitmap, "index_bytes": index_bytes}


# ------------------------------------------------------------------ 等价性验证

def _ink_height(c: Canvas, text: str, pt: int) -> int:
    b = c.d.textbbox((0, 0), text, font=cp.load_font(F_CN, pt))
    return b[3] - b[1]


def verify_equivalence(candidates: list[int], verbose: bool = True
                       ) -> tuple[int, list[str]]:
    """证明「候选集内取最大满足 pt」== `font_for_height` 的二分结果。

    二分返回的是**全局**（6..400）中满足 `高度 ≤ target_h` 的最大 pt；而候选集
    ⊆ 全局且**必含该答案**（普查就是按同一算式收集的），故候选集内的最大值必然
    与之相等。这里把结论用穷举（签名去重后）实测一遍，不只靠推理。

    ⚠️ **前提**：每个候选 pt 都能**完整画出**该串。这里两边都用完整字体渲染，
    所以前提成立；但**固件侧每组字符集不同**，前提可能不成立 ⇒ 本验证只是
    **必要条件**，固件端还必须加 `missing == 0` 判据（见 docs/03 §4.1）。
    完整验证见 tools/font_missing_test.cc 用例 ⑤。
    """
    c = Canvas()
    cand = sorted(candidates)
    cache: dict[tuple[str, int], int] = {}
    checks = 0
    fails: list[str] = []

    def ih(t: str, pt: int) -> int:
        k = (t, pt)
        v = cache.get(k)
        if v is None:
            v = _ink_height(c, t, pt)
            cache[k] = v
        return v

    for name, texts, hs in INK_REGIONS:
        for rep in _by_signature(texts).values():
            for h in hs:
                py = c.font_for_height(rep, F_CN, h).size
                ok = max((pt for pt in cand if ih(rep, pt) <= h), default=None)
                checks += 1
                if ok != py:
                    fails.append(f"{name} | {rep!r} h={h} 二分={py} 候选={ok}")

    if verbose:
        verdict = "全部一致 ✅" if not fails else f"{len(fails)} 例不一致 ❌"
        print(f"等价性验证（候选集查找 vs 二分）：{checks} 例，{verdict}")
        for f in fails[:10]:
            print("   " + f)
    return checks, fails


def main() -> None:
    argv = sys.argv[1:]
    verbose = "--quiet" not in argv

    # 先做一致性检查，不合格就不落盘 —— 免得把坏清单写进 font_charset.json。
    if verbose:
        print("【零】班次名形态表 ↔ 字符集 一致性")
    problems = check_shift_vocabulary(verbose=verbose)
    if problems:
        raise SystemExit("班次名字符集与已知形态不一致，未写出清单。")
    if verbose:
        print()

    r = build_plan(verbose=verbose)
    if "--verify" in argv:
        print()
        verify_equivalence([g["pt"] for g in r["groups"]])


if __name__ == "__main__":
    main()
