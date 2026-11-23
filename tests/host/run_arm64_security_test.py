#!/usr/bin/env python3
"""验证 ARMv9 PAC / BTI / MTE2 安全硬化及 GP/Tagged 页表编码主机单元测试。"""

from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"


def main():
    with tempfile.TemporaryDirectory(prefix="nos-arm64-sec-test-") as tmp:
        exe = Path(tmp) / "test_arm64_security"
        subprocess.run(
            [
                os.environ.get("CC", "cc"),
                "-std=gnu11",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-D__HOST_TEST__=1",
                "-I", str(TEST_DIR / "stubs"),
                "-I", str(ROOT / "include"),
                "-I", str(ROOT / "arch" / "arm64" / "include"),
                str(ROOT / "arch" / "arm64" / "mm" / "mmu.c"),
                str(ROOT / "arch" / "arm64" / "cpu" / "security.c"),
                str(TEST_DIR / "test_arm64_security.c"),
                "-o", str(exe),
            ],
            check=True,
        )
        subprocess.run([str(exe)], check=True, timeout=15)


if __name__ == "__main__":
    main()
