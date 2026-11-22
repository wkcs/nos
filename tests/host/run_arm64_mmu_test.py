#!/usr/bin/env python3
"""运行 ARM64 Stage-1 硬件页表、W^X 策略与 16-bit ASID 主机单元测试。"""

from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"


def main():
    with tempfile.TemporaryDirectory(prefix="nos-arm64-mmu-") as tmp:
        exe = Path(tmp) / "test_arm64_mmu"
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
                str(ROOT / "arch" / "arm64" / "mm" / "mmu.c"),
                str(TEST_DIR / "test_arm64_mmu.c"),
                "-o",
                str(exe),
            ],
            check=True,
        )
        subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == "__main__":
    main()
