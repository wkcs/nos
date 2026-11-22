#!/usr/bin/env python3
"""运行 ARM64 Generic Timer 128-bit 换算、GIC IRQ 分发表与 AI UAPI 主机测试。"""

from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"


def main():
    with tempfile.TemporaryDirectory(prefix="nos-arm64-gic-timer-") as tmp:
        exe = Path(tmp) / "test_arm64_gic_timer"
        subprocess.run(
            [
                os.environ.get("CC", "cc"),
                "-std=gnu11",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-D__HOST_TEST__",
                "-I",
                str(ROOT / "include"),
                "-I",
                str(ROOT / "arch" / "arm64" / "include"),
                str(ROOT / "arch" / "arm64" / "kernel" / "timer.c"),
                str(ROOT / "arch" / "arm64" / "kernel" / "irq.c"),
                str(TEST_DIR / "test_arm64_gic_timer.c"),
                "-o",
                str(exe),
            ],
            check=True,
        )
        subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == "__main__":
    main()
