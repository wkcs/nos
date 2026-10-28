#!/usr/bin/env python3
"""Test kernel log records against the actual FIFO and a bounded backend."""

from pathlib import Path
import os
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"


def main():
    with tempfile.TemporaryDirectory(prefix="nos-log-ring-test-") as tmp:
        exe = Path(tmp) / "test_log_ring"
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-fno-builtin",
             "-DCONFIG_LOG_BUFFERED", "-DCONFIG_LOG_FIFO_BUF_SIZE=32",
             "-DCONFIG_LOG_MESSAGE_SIZE=8", "-I", str(TEST_DIR / "stubs"),
             "-I", str(ROOT / "include"), str(ROOT / "kernel/printk/log.c"), str(ROOT / "kernel/printk/console_backend.c"),
             str(ROOT / "libs/kfifo.c"), str(TEST_DIR / "test_log_ring.c"),
             "-o", str(exe)], check=True,
        )
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
