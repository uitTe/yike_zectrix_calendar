"""cal_font 缺字跳过（B 方案）的宿主侧验证驱动。

一条命令走完：编译 → 运行 → 透传退出码。

`cal_font.cc` 是纯逻辑（不碰 IDF 外设），只依赖 `esp_log.h` —— 由 `tools/stub/`
里的替身接住，于是能在 PC 上用 g++ 编一遍，把「缺字跳过」这条分支真正跑起来。
固件构建不受影响：`-I tools/stub` 只加在这里。

⚠️ `cal_font_data.c` 必须用 **gcc 按 C 编**（不能交给 g++）：
C 里文件作用域的 `const size_t kCalFontGroupCount` 是外部链接，而 C++ 里是内部
链接 —— 混着编会直接 undefined reference。

用法：
    tools/.venv/Scripts/python.exe tools/font_missing_test.py
    tools/.venv/Scripts/python.exe tools/font_missing_test.py --keep
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GCC_CANDIDATES = [
    Path(r"D:\development_tools\msys64\ucrt64\bin\gcc.exe"),
    Path(r"C:\msys64\ucrt64\bin\gcc.exe"),
]
BUILD_DIR = ROOT / ".tmp_cc"
BIN = BUILD_DIR / "font_missing_test.exe"

C_SRC = [
    ROOT / "app" / "components" / "cal_font" / "cal_font_data.c",
    ROOT / "tools" / "stub" / "esp_log_stub.c",
]
CXX_SRC = [
    ROOT / "app" / "components" / "cal_font" / "cal_font.cc",
    ROOT / "tools" / "font_missing_test.cc",
]
INC = [
    ROOT / "app" / "components" / "cal_font" / "include",
    ROOT / "tools" / "stub",
]

WARN = ["-Wall", "-Wextra", "-Wno-unused-parameter"]


def find_tool(name: str) -> Path | None:
    for gcc in GCC_CANDIDATES:
        cand = gcc.parent / name
        if cand.exists():
            return cand
    w = shutil.which(name)
    return Path(w) if w else None


def main() -> int:
    # 子进程的 stdout 直接写 fd，本进程的被缓冲 → 不关行缓冲的话打印会乱序
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except AttributeError:
        pass

    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="保留编译产物")
    args = ap.parse_args()

    gcc = find_tool("gcc.exe")
    gxx = find_tool("g++.exe")
    if gcc is None or gxx is None:
        print("❌ 找不到宿主 gcc / g++（期望 D:\\development_tools\\msys64\\ucrt64\\bin）")
        return 2

    # ⚠️ 必须把 ucrt64\bin 放进子进程 PATH：否则 gcc/g++ 加载不到自己的 DLL，
    #    Windows 上会**静默退出**（无输出、无错误码），极易误判成「编译失败」。
    env = dict(os.environ)
    env["PATH"] = str(gcc.parent) + os.pathsep + env.get("PATH", "")

    BUILD_DIR.mkdir(exist_ok=True)
    inc = []
    for i in INC:
        inc += ["-I", str(i)]

    objs: list[Path] = []
    steps = (
        [(gcc, "-std=c11", "-O2") + tuple(WARN) + tuple(inc) + (str(s),) for s in C_SRC]
        + [(gxx, "-std=c++17", "-O2") + tuple(WARN) + tuple(inc) + (str(s),)
           for s in CXX_SRC]
    )

    print("[1/2] 编译（宿主工具链）：")
    print(f"      C   : {gcc}")
    print(f"      C++ : {gxx}")
    for cmd in steps:
        src = Path(cmd[-1])
        obj = BUILD_DIR / (src.stem + ".o")
        full = list(cmd) + ["-c", "-o", str(obj)]
        r = subprocess.run(full, cwd=ROOT, env=env)
        if r.returncode != 0:
            print(f"❌ 编译失败：{src.name}")
            return r.returncode
        objs.append(obj)

    print("[2/2] 链接并运行：\n")
    r = subprocess.run([str(gxx)] + [str(o) for o in objs] + ["-o", str(BIN)],
                       cwd=ROOT, env=env)
    if r.returncode != 0:
        print("❌ 链接失败")
        return r.returncode

    r = subprocess.run([str(BIN)], cwd=ROOT, env=env)

    if not args.keep:
        for o in objs:
            try:
                o.unlink()
            except OSError:
                pass
        try:
            BIN.unlink()
            if not any(BUILD_DIR.iterdir()):
                BUILD_DIR.rmdir()
        except OSError:
            pass
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
