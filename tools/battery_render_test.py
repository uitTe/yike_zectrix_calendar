"""验证电量显示的真实渲染代码、边界值与全部页面状态。"""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent


def main():
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    cases = ((0, "normal"), (19, "normal"), (20, "normal"), (75, "normal"),
             (99, "normal"), (100, "normal"), (0, "charging"), (75, "charging"),
             (100, "charging"), (10, "full"), (0, "absent"), (0, "fault"))
    for percent, state in cases:
        print(f"电量输入 {percent}%，状态 {state}", flush=True)
        subprocess.run([sys.executable, str(ROOT / "tools/cal_render_test.py"),
                        "--blank-schedule", "--battery-percent", str(percent),
                        "--battery-state", state], cwd=ROOT, env=environment, check=True)
    print(f"电量渲染验证通过：{len(cases)} 组电源输入 × 17 个页面，全部逐像素一致。")


if __name__ == "__main__":
    main()
