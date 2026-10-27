#!/usr/bin/env python3
"""Exercise the production PL011 polling backend using simulated MMIO."""

from pathlib import Path
import os
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"


def main():
    with tempfile.TemporaryDirectory(prefix="nos-pl011-test-") as tmp:
        exe = Path(tmp) / "test_pl011_uart"
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=gnu11", "-O1",
             "-I", str(TEST_DIR / "stubs"), "-I", str(ROOT / "include"),
             str(TEST_DIR / "test_pl011_uart.c"), "-o", str(exe)], check=True,
        )
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
