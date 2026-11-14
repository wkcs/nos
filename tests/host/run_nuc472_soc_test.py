#!/usr/bin/env python3
"""运行 NUC472 SoC 寄存器布局与时钟主机回归测试。"""

from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
CC = "clang" if subprocess.run(["which", "clang"], capture_output=True).returncode == 0 else "gcc"


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / "test_nuc472_soc"
        cmd = [
            CC,
            "-Wall",
            "-Wextra",
            "-Werror",
            "-std=gnu11",
            "-I",
            str(ROOT / "soc/nuc472/include"),
            str(ROOT / "tests/host/test_nuc472_soc.c"),
            "-o",
            str(exe),
        ]
        subprocess.run(cmd, check=True)
        subprocess.run([str(exe)], check=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
