#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
idf.py —— ESP-IDF 构建入口（Windows 专用包装器）

为什么需要这个包装器（三个坑，全部实测踩过）：

  1) MSYSTEM 硬拒
     ESP-IDF 的 idf_tools.py 开头检测 MSYSTEM，一旦发现就 fatal 退出
     ("MSys/Mingw is not supported")。Git Bash 必然带 MSYSTEM。

  2) IDF_TOOLS_PATH 丢失 -> 去 C 盘找 venv
     官方 export 脚本不含 IDF_TOOLS_PATH；工具链装在非默认位置（本项目在 D 盘）时，
     idf.py 会回退到 %USERPROFILE%\\.espressif 并报 "python.exe doesn't exist"。

  3) PATH 无法通过 shell 传递
     Windows 进程环境块里 'Path' / 'PATH' 可能并存，shell 里改 PATH 后子进程
     读到的仍是旧值（实测：普通变量能传，PATH 被还原），导致 cmake 报
     "unable to find a build program corresponding to Ninja"。
     本脚本在 Python 侧显式构造 env 传给子进程，彻底绕开该问题。

用法（Git Bash / cmd / PowerShell 均可）:
    python tools/idf.py build
    python tools/idf.py set-target esp32s3
    python tools/idf.py -C firmware flash monitor
"""
import os
import shutil
import subprocess
import sys

# --- 固定路径（全部在 D 盘，不污染 C 盘） ---
IDF_PATH = r"D:\development_tools\esp\esp-idf"
TOOLS_PATH = r"D:\development_tools\esp\espressif"
PY_ENV = os.path.join(TOOLS_PATH, "python_env", "idf5.4_py3.13_env")
IDF_PY = os.path.join(PY_ENV, "Scripts", "python.exe")

MSYS_KEYS = ("MSYSTEM", "MSYSCON", "MINGW_PREFIX", "MSYS")


def _clean_base_env() -> dict:
    env = dict(os.environ)
    for k in MSYS_KEYS:
        env.pop(k, None)
    env["IDF_PATH"] = IDF_PATH
    env["IDF_TOOLS_PATH"] = TOOLS_PATH
    env["IDF_PYTHON_ENV_PATH"] = PY_ENV
    return env


def _scan_tool_dirs() -> list:
    """回退方案：扫描 tools 目录，收集所有含可执行文件（或 bin/）的目录。"""
    dirs, root = [], os.path.join(TOOLS_PATH, "tools")
    if not os.path.isdir(root):
        return dirs
    for name in sorted(os.listdir(root)):
        tool_root = os.path.join(root, name)
        if not os.path.isdir(tool_root):
            continue
        for ver in sorted(os.listdir(tool_root)):
            ver_dir = os.path.join(tool_root, ver)
            if not os.path.isdir(ver_dir):
                continue
            try:
                inner = [os.path.join(ver_dir, x) for x in sorted(os.listdir(ver_dir))
                         if os.path.isdir(os.path.join(ver_dir, x))]
            except OSError:
                inner = []
            for cand in [ver_dir] + inner:
                bin_dir = os.path.join(cand, "bin")
                if os.path.isdir(bin_dir):
                    dirs.append(bin_dir)
                else:
                    try:
                        has_exe = any(f.lower().endswith(".exe") for f in os.listdir(cand))
                    except OSError:
                        has_exe = False
                    if has_exe:
                        dirs.append(cand)
    return dirs


def build_env() -> dict:
    """构造一个干净、完整、可直接交给子进程的环境块。"""
    env = _clean_base_env()
    # 保存原始系统 PATH：export 会用工具链 PATH 覆盖 env['PATH']，
    # 若不留副本，git 等系统工具就会从 PATH 里消失（cmake 会报 "Could NOT find Git"）。
    base_path = env.get("PATH", "")

    # 问 idf_tools.py 要完整环境（它知道所有工具链路径）。
    # --format=key-value 输出形如 PATH=... / IDF_PATH=...
    try:
        r = subprocess.run(
            [IDF_PY, os.path.join(IDF_PATH, "tools", "idf_tools.py"),
             "export", "--format=key-value"],
            capture_output=True, text=True, env=env,
        )
    except OSError as exc:
        raise SystemExit(f"无法执行 idf_tools.py: {exc}")

    if r.returncode == 0 and r.stdout.strip():
        for line in r.stdout.splitlines():
            line = line.strip()
            if "=" in line:
                key, val = line.split("=", 1)
                env[key] = val
    else:
        # 回退：通常是某个必需工具没装（如 riscv32-esp-elf-gdb）导致 export 退出码非 0
        last_err = r.stderr.strip().splitlines()[-1] if r.stderr.strip() else "未知原因"
        print(f"WARNING: idf_tools.py export 失败（{last_err}）", file=sys.stderr)
        print("         改用目录扫描构造 PATH。", file=sys.stderr)

        extra = _scan_tool_dirs()
        env["PATH"] = os.pathsep.join(extra + [env.get("PATH", "")])
        # 这两个变量 export 才会给，回退时手动补
        elf_root = os.path.join(TOOLS_PATH, "tools", "esp-rom-elfs")
        if os.path.isdir(elf_root):
            vers = sorted(os.listdir(elf_root))
            if vers:
                env["ESP_ROM_ELF_DIR"] = os.path.join(elf_root, vers[-1])
        oc_root = os.path.join(TOOLS_PATH, "tools", "openocd-esp32")
        if os.path.isdir(oc_root):
            vers = sorted(os.listdir(oc_root))
            if vers:
                env["OPENOCD_SCRIPTS"] = os.path.join(
                    oc_root, vers[-1], "openocd-esp32", "share", "openocd", "scripts")

    # 兜底：idf_tools 不导出这两个，缺了 idf.py 会去 C 盘找 venv
    env["IDF_TOOLS_PATH"] = TOOLS_PATH
    env["IDF_PYTHON_ENV_PATH"] = PY_ENV

    # 把 IDF 自带 Python、工具链目录、原始系统 PATH 合成单一路径，去重后统一成 'PATH'。
    # 注意：必须先取出旧值再删键——否则会把 export 注入的工具链路径一起丢掉。
    parts = [os.path.join(PY_ENV, "Scripts"), os.path.join(IDF_PATH, "tools")]
    for chunk in (env.get("PATH", ""), base_path):
        for p in chunk.split(os.pathsep):
            p = p.strip()
            if p and p not in parts:
                parts.append(p)

    for key in [k for k in env if k.upper() == "PATH"]:
        del env[key]
    env["PATH"] = os.pathsep.join(parts).rstrip(os.pathsep)

    return env


def verify_tools(env: dict) -> None:
    """自检：cmake / ninja 必须可解析，否则提前给出明确信号。"""
    missing = []
    for tool in ("cmake", "ninja", "ccache"):
        found = shutil.which(tool, path=env.get("PATH", ""))
        print(f"  [check] {tool:8s} -> {found or 'NOT FOUND'}")
        if not found:
            missing.append(tool)
    if missing:
        print(f"WARNING: PATH 中未找到 {', '.join(missing)}", file=sys.stderr)


def main() -> int:
    if not os.path.isfile(IDF_PY):
        raise SystemExit(f"找不到 IDF 专用 Python: {IDF_PY}")

    env = build_env()
    verify_tools(env)

    cmd = [IDF_PY, os.path.join(IDF_PATH, "tools", "idf.py")] + sys.argv[1:]
    print(">>> " + " ".join(cmd))
    print()
    sys.stdout.flush()
    return subprocess.call(cmd, env=env)


if __name__ == "__main__":
    sys.exit(main())
