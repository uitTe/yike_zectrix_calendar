"""台历页点阵字库生成器（PC 侧）。

把 Noto Sans SC（wght=700）按 `tools/pt_census.py` 普查出的**字号清单**
（`tools/testdata/font_charset.json`）渲染成 1bpp 点阵，写成固件可用的 C 数据文件
`app/components/cal_font/cal_font_data.c`。

⚠️ 字号清单是**自动普查**的产物，不是手写的 —— 渲染器按墨迹高二分选字号，
同一区域会落在一批不同 pt 上，手写必漏。详见 `tools/pt_census.py`。

每个字形存两个盒子，缺一不可：

  * **度量盒**（bdx/bdy/bw/bh）来自 Pillow 的 `textbbox()` —— 渲染器算整串墨迹
    包围盒、据此居中对齐用的就是这个盒子；
  * **点阵盒**（idx/idy/iw/ih + 位图）来自实际栅格化后的墨迹 —— 真正落到画布上的
    像素范围，它与度量盒常常差 1 像素（FreeType 的字形度量范围不等于位图范围）。

固件端必须同时用这两个盒子：拿度量盒对齐锚点、拿点阵盒贴图，才能和 PC 侧渲染器
逐像素一致。tools/font_verify.py 会把这个模型与 Pillow 的实际绘制做逐像素比对。

用法：python tools/font_bitmap.py
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from PIL import Image, ImageDraw  # noqa: E402

import calendar_preview as cp  # noqa: E402

_HERE = Path(__file__).resolve().parent
OUT_FILE = _HERE.parent / "app" / "components" / "cal_font" / "cal_font_data.c"
PLAN_FILE = _HERE / "testdata" / "font_charset.json"

MARGIN_FACTOR = 2          # 画布留白 = pt * MARGIN_FACTOR + 32
INK_THRESHOLD = 127        # 与 calendar_preview.save_1bpp 的二值化阈值一致

GLYPH_CACHE: dict[tuple[str, int], dict] = {}


def glyph_metrics(ch: str, pt: int) -> dict:
    """渲染单字，返回度量盒、点阵盒与位图行。

    返回：advance / bdx / bdy / bw / bh / idx / idy / iw / ih / rows
    """
    key = (ch, pt)
    cached = GLYPH_CACHE.get(key)
    if cached is not None:
        return cached

    font = cp.load_font(cp.FONT_FILE, pt)
    advance = font.getlength(ch)
    if abs(advance - round(advance)) > 1e-6:
        raise SystemExit(f"失败：pt{pt} 的「{ch}」步进不是整像素（{advance}），"
                         "固件侧无法用整数笔位复现")
    advance = int(round(advance))

    margin = pt * MARGIN_FACTOR + 32
    size = margin * 2 + pt * 3
    img = Image.new("L", (size, size), 255)
    draw = ImageDraw.Draw(img)
    origin = (margin, margin)
    draw.text(origin, ch, font=font, fill=0)

    metric = draw.textbbox(origin, ch, font=font)
    # 墨迹（灰度 <= 阈值）置 255、背景置 0，便于用 getbbox() 取实际点阵范围
    ink = img.point(lambda p: 255 if p <= INK_THRESHOLD else 0)
    ink_box = ink.getbbox()
    if ink_box is None:
        ink_box = (margin, margin, margin, margin)
    if ink_box[0] <= 0 or ink_box[1] <= 0 or ink_box[2] >= size or ink_box[3] >= size:
        raise SystemExit(f"失败：pt{pt}「{ch}」的墨迹触到画布边界，留白不足")

    pixels = ink.load()
    rows: list[int] = []
    for y in range(ink_box[1], ink_box[3]):
        value = 0
        bits = 0
        for x in range(ink_box[0], ink_box[2]):
            if pixels[x, y]:
                value |= 0x80 >> bits
            bits += 1
            if bits == 8:
                rows.append(value)
                value = 0
                bits = 0
        if bits:
            rows.append(value)

    result = {
        "advance": advance,
        "bdx": metric[0] - margin,
        "bdy": metric[1] - margin,
        "bw": metric[2] - metric[0],
        "bh": metric[3] - metric[1],
        "idx": ink_box[0] - margin,
        "idy": ink_box[1] - margin,
        "iw": ink_box[2] - ink_box[0],
        "ih": ink_box[3] - ink_box[1],
        "rows": rows,
    }
    GLYPH_CACHE[key] = result
    return result


def load_plan() -> list[tuple[int, list[str]]]:
    """读字号清单，返回 [(pt, 码点升序的字表)]，行按 pt 降序。

    ⚠️ 清单是 **`tools/pt_census.py` 自动普查**出来的，**不要手写**。
    原因：`Canvas.font_for_height()` 按墨迹高二分选字号，同一区域会随内容落在
    一批不同 pt 上（实测「主区时段」出现在 pt17–pt38 共 20 档、农历在 pt15–pt21
    共 9 档），手写清单必然会漏（早期 `charset_plan.py` 的手写版就漏了 20 档）。
    普查还顺带证明了「固件端用候选集查找 == PC 侧二分」（`--verify`）。

    JSON 不存在时自动跑一次普查生成。
    """
    if not PLAN_FILE.exists():
        print(f"字号清单不存在，先跑普查生成 {PLAN_FILE.name} …")
        subprocess.run([sys.executable, str(_HERE / "pt_census.py"), "--quiet"],
                       check=True, cwd=str(_HERE.parent))
    data = json.loads(PLAN_FILE.read_text(encoding="utf-8"))
    return [(g["pt"], sorted(g["chars"], key=ord)) for g in data["groups"]]


def emit_hex(rows: list[int], indent: str = "    ") -> list[str]:
    lines = []
    for i in range(0, len(rows), 12):
        chunk = rows[i:i + 12]
        lines.append(indent + "".join(f"0x{b:02X}, " for b in chunk).rstrip())
    return lines


def main() -> None:
    groups = load_plan()
    out: list[str] = []
    out.append("// 由 tools/font_bitmap.py 生成，请不要手工修改。")
    out.append("// 字源：Noto Sans SC（SIL OFL 1.1），wght=700；二值化阈值 127。")
    out.append("// 字号组来自 tools/pt_census.py 的自动普查（tools/testdata/font_charset.json），")
    out.append("// 请不要手工修改本文件，也不要手改字号清单 —— 改动请改普查脚本后重跑。")
    out.append("")
    out.append('#include "cal_font.h"')
    out.append("")

    total_bitmap = 0
    summary = []
    for pt, chars in groups:
        pool: list[int] = []
        index_lines: list[str] = []
        for ch in chars:
            g = glyph_metrics(ch, pt)
            for field in ("bdx", "bdy", "idx", "idy"):
                if not -128 <= g[field] <= 127:
                    raise SystemExit(f"失败：pt{pt}「{ch}」的 {field}={g[field]} "
                                     "超出 int8 范围，需要加宽字段")
            for field in ("bw", "bh", "iw", "ih", "advance"):
                if not 0 <= g[field] <= 255:
                    raise SystemExit(f"失败：pt{pt}「{ch}」的 {field}={g[field]} "
                                     "超出 uint8 范围，需要加宽字段")
            offset = len(pool)
            pool.extend(g["rows"])
            if offset > 0xFFFF:
                raise SystemExit(f"失败：pt{pt} 的点阵池超过 64 KB")
            index_lines.append(
                f"    {{0x{ord(ch):04X}, {g['bdx']:>4}, {g['bdy']:>4}, {g['bw']:>2}, "
                f"{g['bh']:>2}, {g['idx']:>4}, {g['idy']:>4}, {g['iw']:>2}, {g['ih']:>2}, "
                f"{g['advance']:>2}, {offset:>5}}},  // {ch}"
            )
        total_bitmap += len(pool)
        summary.append((pt, len(chars), len(pool)))

        out.append(f"// pt{pt}：{len(chars)} 字，点阵 {len(pool)} B")
        out.append(f"static const uint8_t kBitmapPt{pt}[] = {{")
        out.extend(emit_hex(pool))
        out.append("};")
        out.append("")
        out.append(f"static const CalGlyph kGlyphsPt{pt}[] = {{")
        out.extend(index_lines)
        out.append("};")
        out.append("")

    out.append("const CalFontGroup kCalFontGroups[] = {")
    for pt, count, _size in summary:
        out.append(f"    {{{pt:>3}, {count:>3}, kGlyphsPt{pt}, kBitmapPt{pt}}},")
    out.append("};")
    out.append("")
    out.append("const size_t kCalFontGroupCount = "
               "sizeof(kCalFontGroups) / sizeof(kCalFontGroups[0]);")
    out.append("")

    OUT_FILE.parent.mkdir(parents=True, exist_ok=True)
    OUT_FILE.write_text("\n".join(out), encoding="utf-8")

    index_bytes = sum(count * 14 for _pt, count, _size in summary)
    print(f"写入 {OUT_FILE}")
    print(f"{'字号':>4} {'字数':>5} {'点阵':>10}")
    for pt, count, size in summary:
        print(f"{pt:>4} {count:>5} {size:>7} B")
    print(f"点阵合计 {total_bitmap} B，索引约 {index_bytes} B，"
          f"合计约 {(total_bitmap + index_bytes) / 1024:.1f} KB")


if __name__ == "__main__":
    main()
