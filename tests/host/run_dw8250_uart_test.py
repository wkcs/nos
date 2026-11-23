#!/usr/bin/env python3
"""验证 DesignWare DW-8250 (RK3588 UART2) 串口驱动主机单元测试。"""

from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"


def main():
    with tempfile.TemporaryDirectory(prefix="nos-dw8250-test-") as tmp:
        exe = Path(tmp) / "test_dw8250_uart"
        subprocess.run(
            [
                os.environ.get("CC", "cc"),
                "-std=gnu11",
                "-O1",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-D__HOST_TEST__=1",
                "-I", str(TEST_DIR / "stubs"),
                "-I", str(ROOT / "include"),
                str(TEST_DIR / "test_dw8250_uart.c"),
                "-o", str(exe),
            ],
            check=True,
        )
        subprocess.run([str(exe)], check=True, timeout=15)


if __name__ == "__main__":
    main()
