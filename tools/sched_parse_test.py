"""排班解析器的宿主侧验证驱动。

一条命令走完：用宿主编译器编译 `cal_sched_parse.c` + cJSON → 用合成样例
跑 `tools/sched_parse_test.c` 里的全部断言 → 透传退出码。样例结构取自实测响应，
班次名与分组编号是虚构值。

能这么测是因为解析器是**纯 C99、零 IDF 依赖**（`tools/sched_parse_test.c` 与
`cal_sched_parse.c` 都只依赖 cJSON 与标准库）。cJSON 源码直接取 ESP-IDF 自带那份，
PC 与设备用同一个实现 —— 换一份第三方 cJSON 测出来的结论不等于设备上的结论。

用法：
    tools/.venv/Scripts/python.exe tools/sched_parse_test.py
    tools/.venv/Scripts/python.exe tools/sched_parse_test.py --keep   # 保留中间产物
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
IDF_CJSON = Path(r"D:\development_tools\esp\esp-idf\components\json\cJSON")
BUILD_DIR = ROOT / ".tmp_cc"
BIN = BUILD_DIR / "sched_parse_test.exe"
SAMPLE = ROOT / "tools" / "testdata" / "agc_response_20261022.json"

SRC = [
    ROOT / "tools" / "sched_parse_test.c",
    ROOT / "app" / "components" / "cal_sched" / "cal_sched_parse.c",
    IDF_CJSON / "cJSON.c",
]
INC = [
    ROOT / "app" / "components" / "cal_sched" / "include",
    IDF_CJSON,
]


def find_gcc() -> Path | None:
    for p in GCC_CANDIDATES:
        if p.exists():
            return p
    w = shutil.which("gcc")
    return Path(w) if w else None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="保留编译产物")
    args = ap.parse_args()

    for path in [SAMPLE, IDF_CJSON / "cJSON.c"]:
        if not path.exists():
            print(f"❌ 缺少文件：{path}")
            return 2

    gcc = find_gcc()
    if gcc is None:
        print("❌ 找不到宿主 gcc（期望 D:\\development_tools\\msys64\\ucrt64\\bin\\gcc.exe）")
        return 2

    BUILD_DIR.mkdir(exist_ok=True)
    cmd = [str(gcc), "-O2", "-std=c11", "-Wall", "-Wextra"]
    for i in INC:
        cmd += ["-I", str(i)]
    cmd += [str(s) for s in SRC]
    cmd += ["-o", str(BIN)]

    # gcc.exe 必须能加载自己目录下的 DLL —— 不把它所在的 bin 加进 PATH，
    # Windows 上会静默退出（无输出、无错误码），很容易误判成「编译没过」。
    env = dict(os.environ)
    env["PATH"] = str(gcc.parent) + os.pathsep + env.get("PATH", "")

    print("[1/2] 编译（宿主编译器）：")
    print("      " + str(gcc))
    r = subprocess.run(cmd, cwd=ROOT, env=env)
    if r.returncode != 0:
        print("❌ 编译失败")
        return r.returncode

    print("[2/2] 运行断言：\n")
    r = subprocess.run([str(BIN), str(SAMPLE)], cwd=ROOT, env=env)

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
