#!/usr/bin/env python3
"""运行 NUC472 片上外设 (PWM, EADC, TIMER, RTC, WDT, EMAC Probe) 主机回归测试。"""

from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"
CC = "clang" if subprocess.run(["which", "clang"], capture_output=True).returncode == 0 else "gcc"


def main():
    with tempfile.TemporaryDirectory(prefix="nos-nuc472-periph-test-") as tmp:
        exe = Path(tmp) / "test_nuc472_periph"
        cmd = [
            CC,
            "-Wall",
            "-Wextra",
            "-Werror",
            "-std=gnu11",
            "-I",
            str(TEST_DIR / "stubs"),
            "-I",
            str(ROOT / "include"),
            "-I",
            str(ROOT / "arch/arm/include"),
            "-I",
            str(ROOT / "soc/nuc472/include"),
            str(TEST_DIR / "test_nuc472_periph.c"),
            "-o",
            str(exe),
        ]
        subprocess.run(cmd, check=True)
        subprocess.run([str(exe)], check=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
