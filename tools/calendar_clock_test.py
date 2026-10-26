"""编译并验证实际使用的每日零点计算代码。"""
from pathlib import Path
import os
import subprocess

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / ".tmp_clock"
TOOLCHAIN = Path(r"D:\development_tools\msys64\ucrt64\bin")


def main():
    BUILD.mkdir(exist_ok=True)
    env = dict(os.environ)
    env["PATH"] = str(TOOLCHAIN) + os.pathsep + env.get("PATH", "")
    include = ["-I", str(ROOT / "app/components/cal_almanac/include"),
               "-I", str(ROOT / "app/main")]
    objects = []
    for name in ("cal_almanac", "cal_lunar_table"):
        obj = BUILD / f"{name}.o"
        subprocess.run([str(TOOLCHAIN / "gcc.exe"), "-std=c11", "-O2",
                        "-Wall", "-Wextra", *include, "-c",
                        str(ROOT / f"app/components/cal_almanac/{name}.c"),
                        "-o", str(obj)], env=env, check=True)
        objects.append(str(obj))
    executable = BUILD / "calendar_clock_test.exe"
    subprocess.run([str(TOOLCHAIN / "g++.exe"), "-std=c++17", "-O2",
                    "-Wall", "-Wextra", *include,
                    str(ROOT / "tools/calendar_clock_test.cc"), *objects,
                    "-o", str(executable)], env=env, check=True)
    subprocess.run([str(executable)], env=env, check=True)
    button_test = BUILD / "calendar_button_test.exe"
    subprocess.run([str(TOOLCHAIN / "g++.exe"), "-std=c++17", "-O2",
                    "-Wall", "-Wextra", *include,
                    str(ROOT / "tools/calendar_button_test.cc"),
                    "-o", str(button_test)], env=env, check=True)
    subprocess.run([str(button_test)], env=env, check=True)


if __name__ == "__main__":
    main()
