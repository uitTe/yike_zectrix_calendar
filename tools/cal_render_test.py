"""台历页渲染器（固件 C）与权威渲染器（PC Python）的**逐像素**比对。

为什么必须这么做：渲染器是全页唯一「坐标错 1px 就整屏歪、字号选错一档就整块变形」
的地方。肉眼看图只能发现「明显坏了」，发现不了「反白签整体右移 2px」「时段用的
是 pt24 而 PC 用 pt25」「最坏节日组合退档退到另一档」这类问题。这里比的是
**15000 字节的最终帧**，等价于把设备屏幕直接和 PC 出图叠加。

链路刻意走「完整链路」：C 侧吃**原始** am/pm（内部自己加前缀），Python 侧也走
`with_prefix()` → `build()`，与 `calendar_A_final.py --json` 的取数链路一致
（回归铁律 ①：直接给 build() 传裸值会漏 with_prefix，造成 ~4600 px 假差异）。

用法：
    tools/.venv/Scripts/python.exe tools/cal_render_test.py
    tools/.venv/Scripts/python.exe tools/cal_render_test.py --keep   # 保留中间产物
"""

from __future__ import annotations

import argparse
import datetime as dt
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))

import calendar_A_final as caf  # noqa: E402
import calendar_preview as cp  # noqa: E402
from PIL import Image  # noqa: E402

FRAME_W, FRAME_H = cp.W, cp.H

GCC_CANDIDATES = [
    Path(r"D:\development_tools\msys64\ucrt64\bin\gcc.exe"),
    Path(r"C:\msys64\ucrt64\bin\gcc.exe"),
]
BUILD_DIR = ROOT / ".tmp_render"
BIN = BUILD_DIR / "cal_render_probe.exe"

# --- 编译单元（顺序无关） ---------------------------------------------------
C_SRC = [
    ROOT / "app" / "components" / "cal_font" / "cal_font_data.c",
    ROOT / "app" / "components" / "cal_almanac" / "cal_almanac.c",
    ROOT / "app" / "components" / "cal_almanac" / "cal_lunar_table.c",
    ROOT / "tools" / "stub" / "esp_log_stub.c",
]
CXX_SRC = [
    ROOT / "app" / "components" / "cal_font" / "cal_font.cc",
    ROOT / "app" / "components" / "cal_render" / "cal_render.cc",
    ROOT / "tools" / "cal_render_probe.cc",
]
INC = [
    ROOT / "app" / "components" / "cal_font" / "include",
    ROOT / "app" / "components" / "cal_almanac" / "include",
    ROOT / "app" / "components" / "cal_render" / "include",
    ROOT / "tools" / "stub",
]

WARN = ["-Wall", "-Wextra", "-Wno-unused-parameter"]

# --- 用例 -------------------------------------------------------------------
# 覆盖：节气 / 节日 / 撞车（最宽签行）/ 清明让位 / 休息日 / 拉取失败 /
#       夜班前缀 / 全角括号 / 只有一段时段 / 自带前缀 / 最宽标题。
CASES: list[tuple[str, str, str, str]] = [
    ("2026-09-23", "9DP1", "08:00-12:00", "14:00-18:00"),          # 秋分
    ("2026-09-24", "9DP1", "08:00-12:00", "14:00-18:00"),          # 无节气无节日
    ("2026-09-25", "9DP1", "08:00-12:00", "14:00-18:00"),          # 中秋节
    ("2026-03-20", "9NP1", "08:00-12:00", "14:00-18:00"),          # 春分 = 龙抬头
    ("2038-02-04", "9DP1", "08:00-12:00", "14:00-18:00"),          # 立春 = 春节
    ("2026-04-05", "9DP1", "08:00-12:00", "14:00-18:00"),          # 清明让位
    ("2031-10-01", "9DP1", "08:00-12:00", "14:00-18:00"),          # 中秋 + 国庆（签行最宽）
    ("2031-01-01", "9DP2", "08:00-12:00", "14:00-18:00"),          # 腊八节 + 元旦
    ("2037-02-14", "9DP1", "08:00-12:00", "14:00-18:00"),          # 除夕 + 情人节
    ("2026-06-15", "休", "", ""),                                   # 休息日
    ("2026-08-08", "", "", ""),                                     # 拉取失败（无常量缓存）
    ("2026-09-26", "9NP1", "20:00-24:00", "00:00-04:00"),          # 夜班：前缀按开始时间
    ("2026-09-27", "9DP0（验车）", "08:00-12:00", "14:00-18:00"),   # 全角括号 → 归一化
    ("2026-09-28", "9DP1(值班)", "08:00-12:00", ""),                # 只有一段时段
    ("2026-09-29", "9DP1(绿)", "上午08:00-12:00", "14:00-18:00"),   # 已带前缀不重复加
    ("2026-12-31", "9DP1", "08:00-12:00", "14:00-18:00"),          # 最宽标题
    ("2027-02-06", "9DP3", "08:00-12:00", "14:00-18:00"),          # 春节
]


def find_tool(name: str) -> Path | None:
    for gcc in GCC_CANDIDATES:
        cand = gcc.parent / name
        if cand.exists():
            return cand
    which = shutil.which(name)
    return Path(which) if which else None


def python_reference(date_s: str, shift: str, am: str, pm: str) -> bytes:
    """权威渲染器的输出，转成与固件完全相同的 1bpp 排布。"""
    d = dt.date.fromisoformat(date_s)
    canvas, _term, _fests, _meta = caf.build(
        d, shift, caf.with_prefix(am), caf.with_prefix(pm))
    bw = canvas.img.point(lambda p: 255 if p > 127 else 0, "1")
    return bw.tobytes()


def diff_report(got: bytes, want: bytes, w: int, h: int) -> tuple[int, tuple[int, int, int, int] | None]:
    """返回 (差异像素数, 差异包围盒)。"""
    stride = (w + 7) // 8
    count = 0
    x0, y0, x1, y1 = w, h, -1, -1
    for y in range(h):
        row = y * stride
        for x in range(w):
            g = (got[row + x // 8] >> (7 - (x & 7))) & 1
            v = (want[row + x // 8] >> (7 - (x & 7))) & 1
            if g != v:
                count += 1
                if x < x0:
                    x0 = x
                if x > x1:
                    x1 = x
                if y < y0:
                    y0 = y
                if y > y1:
                    y1 = y
    if count == 0:
        return 0, None
    return count, (x0, y0, x1, y1)


def save_png(data: bytes, path: Path) -> None:
    Image.frombytes("1", (FRAME_W, FRAME_H), data).save(path)


def main() -> int:
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except AttributeError:
        pass

    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="保留编译产物与出图")
    ap.add_argument("--blank-schedule", action="store_true", help="验证班次和时段保持空白")
    ap.add_argument("--battery-percent", type=int, help="验证指定电量的渲染输入")
    ap.add_argument("--battery-state", choices=("normal", "charging", "full", "absent", "fault"), default="normal")
    ap.add_argument("--wifi-rssi", type=int, help="已连接时的信号强度，配合 --battery-percent 使用")
    ap.add_argument("--wifi-off", action="store_true", help="画断开的交叉斜线")
    args = ap.parse_args()

    show_wifi = args.wifi_off or args.wifi_rssi is not None
    if show_wifi and args.battery_percent is None:
        ap.error("--wifi-rssi / --wifi-off 需要同时给 --battery-percent，信号格锚定在电池左侧")
    wifi_connected = not args.wifi_off
    wifi_rssi = args.wifi_rssi if args.wifi_rssi is not None else 0

    gcc = find_tool("gcc.exe")
    gxx = find_tool("g++.exe")
    if gcc is None or gxx is None:
        print("❌ 找不到宿主 gcc / g++（期望 D:\\development_tools\\msys64\\ucrt64\\bin）")
        return 2

    # ⚠️ 必须把 ucrt64\bin 放进子进程 PATH：否则加载不到 DLL，Windows 上会**静默退出**
    #    （无输出、无错误码），极易误判成「编译失败」。
    env = dict(os.environ)
    env["PATH"] = str(gcc.parent) + os.pathsep + env.get("PATH", "")

    BUILD_DIR.mkdir(exist_ok=True)
    inc: list[str] = []
    for path in INC:
        inc += ["-I", str(path)]

    objs: list[Path] = []
    steps = ([(gcc, "-std=c11", "-O2") + tuple(WARN) + tuple(inc) + (str(s),) for s in C_SRC]
             + [(gxx, "-std=c++17", "-O2") + tuple(WARN) + tuple(inc) + (str(s),)
                for s in CXX_SRC])

    print("[1/3] 编译（宿主工具链）：")
    print(f"      C   : {gcc}")
    print(f"      C++ : {gxx}")
    for cmd in steps:
        src = Path(cmd[-1])
        obj = BUILD_DIR / (src.stem + ".o")
        result = subprocess.run(list(cmd) + ["-c", "-o", str(obj)], cwd=ROOT, env=env)
        if result.returncode != 0:
            print(f"❌ 编译失败：{src.name}")
            return result.returncode
        objs.append(obj)

    result = subprocess.run([str(gxx)] + [str(o) for o in objs] + ["-o", str(BIN)],
                            cwd=ROOT, env=env)
    if result.returncode != 0:
        print("❌ 链接失败")
        return result.returncode

    # ⚠️ 用例走**文件**（UTF-8）而不是命令行参数：Windows 上 argv 按 ANSI 代码页
    #    （GBK）传中文，程序里拿到的已经不是合法 UTF-8 → 字库查不到字形 → 带中文的
    #    用例统统画不出来，看着像渲染器的 bug。
    cases_file = BUILD_DIR / "cases.txt"
    cases_file.write_text("\n".join(f"{d}|{s}|{a}|{p}" for d, s, a, p in CASES) + "\n",
                          encoding="utf-8")

    print("[2/3] 渲染（固件 C 侧）：")
    command = [str(BIN), str(BUILD_DIR), str(cases_file)]
    if args.blank_schedule:
        command.append("--blank-schedule")
    if args.battery_percent is not None:
        command += ["--battery-percent", str(args.battery_percent), "--battery-state",
                    str(("normal", "charging", "full", "absent", "fault").index(args.battery_state))]
    if args.wifi_off:
        command.append("--wifi-off")
    elif args.wifi_rssi is not None:
        command += ["--wifi-connected", "--wifi-rssi", str(args.wifi_rssi)]
    run = subprocess.run(command, cwd=ROOT, env=env,
                         capture_output=True, text=True, encoding="utf-8",
                         errors="replace")
    print(run.stdout, end="")
    if run.returncode != 0:
        print(run.stderr, end="")
        print(f"❌ 探针退出码 {run.returncode}")
        return run.returncode

    print("[3/3] 逐像素比对（固件 C vs Python 权威渲染器）：")
    fails = 0
    for index, (date_s, shift, am, pm) in enumerate(CASES):
        got = (BUILD_DIR / f"case_{index:02d}.bin").read_bytes()
        if args.blank_schedule or args.battery_percent is not None:
            wifi_kwargs = ({"wifi_rssi": wifi_rssi, "wifi_connected": wifi_connected}
                           if show_wifi else {})
            canvas, *_ = caf.build(dt.date.fromisoformat(date_s), shift,
                                   caf.with_prefix(am), caf.with_prefix(pm),
                                   blank_schedule=args.blank_schedule,
                                   battery_percent=args.battery_percent,
                                   battery_state=args.battery_state, **wifi_kwargs)
            want = canvas.img.point(lambda p: 255 if p > 127 else 0, "1").tobytes()
        else:
            want = python_reference(date_s, shift, am, pm)
        if args.battery_percent is not None:
            base, *_ = caf.build(dt.date.fromisoformat(date_s), shift,
                                caf.with_prefix(am), caf.with_prefix(pm),
                                blank_schedule=args.blank_schedule)
            base_bytes = base.img.point(lambda p: 255 if p > 127 else 0, "1").tobytes()
            changed, box = diff_report(got, base_bytes, FRAME_W, FRAME_H)
            assert changed > 0 and box is not None, "电量未绘制"
            # 信号格挂在电池左边缘再往左 8px，整个图形最远到 x≈275（电量文字取最宽的
            # `100%` 时）。主区分隔线在 x=232，所以 272 是安全下限。
            left_limit = 272 if show_wifi else 304
            assert left_limit <= box[0] <= box[2] < 384 and 62 <= box[1] <= box[3] < 78, box
            if show_wifi:
                assert box[0] < 304, "信号格未画到电池左侧"
        if args.blank_schedule:
            image = Image.frombytes("1", (FRAME_W, FRAME_H), got)
            assert image.crop((0, 62, 232, 232)).getextrema() == (255, 255), "时段区域未留空"
            assert image.crop((120, 234, 400, 300)).getextrema() == (255, 255), "班次区域未留空"
        if len(got) != len(want):
            print(f"  ❌ {index:02d} {date_s} 帧长不一致 got={len(got)} want={len(want)}")
            fails += 1
            continue
        if got == want:
            print(f"  ✅ {index:02d} {date_s} {shift or '—'} 全帧零差异")
            continue
        count, box = diff_report(got, want, FRAME_W, FRAME_H)
        fails += 1
        print(f"  ❌ {index:02d} {date_s} {shift or '—'} 差异 {count} px，"
              f"包围盒 x[{box[0]},{box[2]}] y[{box[1]},{box[3]}]")
        save_png(got, BUILD_DIR / f"case_{index:02d}_c.png")
        save_png(want, BUILD_DIR / f"case_{index:02d}_py.png")
        print(f"       出图：{BUILD_DIR.name}/case_{index:02d}_c.png 与 "
              f"case_{index:02d}_py.png")

    print()
    if fails == 0:
        print(f"✅ {len(CASES)} 个用例全部逐像素一致。")
    else:
        print(f"❌ {fails} / {len(CASES)} 个用例不一致（出图已留在 {BUILD_DIR.name}/）。")

    if not args.keep and fails == 0:
        for obj in objs:
            try:
                obj.unlink()
            except OSError:
                pass
        for extra in list(BUILD_DIR.glob("case_*.bin")) + [cases_file]:
            try:
                extra.unlink()
            except OSError:
                pass
        try:
            BIN.unlink()
            if not any(BUILD_DIR.iterdir()):
                BUILD_DIR.rmdir()
        except OSError:
            pass
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
