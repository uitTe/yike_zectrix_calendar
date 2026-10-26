"""历法 C 实现的主机侧验证驱动。

一条命令走完：生成黄金向量 → 用宿主编译器编译 → 运行比对 → 透传退出码。

宿主 gcc 用的是本机已有的 MSYS2 UCRT64 工具链（`D:\\development_tools\\msys64\\ucrt64\\bin\\gcc.exe`），
不需要 ESP-IDF，也不需要装新东西。之所以能这么测，是因为 `cal_almanac.c` 是**纯 C99、零 IDF 依赖**。

用法：
    tools/.venv/Scripts/python.exe tools/almanac_test.py            # 全流程
    tools/.venv/Scripts/python.exe tools/almanac_test.py --no-gen   # 跳过向量重生成（快）
    tools/.venv/Scripts/python.exe tools/almanac_test.py --keep     # 保留中间产物
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
BIN = BUILD_DIR / "almanac_test.exe"

SRC = [
    ROOT / "tools" / "almanac_test.c",
    ROOT / "app" / "components" / "cal_almanac" / "cal_almanac.c",
    ROOT / "app" / "components" / "cal_almanac" / "cal_lunar_table.c",
]
INC = [
    ROOT / "app" / "components" / "cal_almanac" / "include",
    ROOT / "tools" / "testdata",
]


def find_gcc() -> Path | None:
    for p in GCC_CANDIDATES:
        if p.exists():
            return p
    w = shutil.which("gcc")
    return Path(w) if w else None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-gen", action="store_true", help="跳过黄金向量重生成")
    ap.add_argument("--keep", action="store_true", help="保留编译产物")
    args = ap.parse_args()

    py = sys.executable

    if not args.no_gen:
        print("[1/3] 生成黄金向量…")
        r = subprocess.run([py, str(ROOT / "tools" / "almanac_vectors.py")], cwd=ROOT)
        if r.returncode != 0:
            return r.returncode
    else:
        print("[1/3] 跳过向量生成（--no-gen）")

    gcc = find_gcc()
    if gcc is None:
        print("❌ 找不到宿主 gcc（期望 D:\\development_tools\\msys64\\ucrt64\\bin\\gcc.exe）")
        return 2

    BUILD_DIR.mkdir(exist_ok=True)
    cmd = [str(gcc), "-O2", "-std=c11", "-Wall", "-Wextra",
           "-Wno-unused-parameter"]
    for i in INC:
        cmd += ["-I", str(i)]
    cmd += [str(s) for s in SRC]
    cmd += ["-o", str(BIN), "-lm"]

    # ⚠️ gcc.exe 必须能加载自己目录下的 DLL —— 不把它所在的 bin 加进 PATH，
    #    Windows 上会**静默退出**（无输出、无错误码），很容易误判成「编译没过」。
    env = dict(os.environ)
    env["PATH"] = str(gcc.parent) + os.pathsep + env.get("PATH", "")

    print("[2/3] 编译（宿主编译器）：")
    print("      " + str(gcc))
    r = subprocess.run(cmd, cwd=ROOT, env=env)
    if r.returncode != 0:
        print("❌ 编译失败")
        return r.returncode

    print("[3/3] 运行比对：\n")
    r = subprocess.run([str(BIN)], cwd=ROOT, env=env)

    if not args.keep:
        try:
            os.remove(BIN)
            if not any(BUILD_DIR.iterdir()):
                BUILD_DIR.rmdir()
        except OSError:
            pass
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
