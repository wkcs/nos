#!/usr/bin/env python3
"""Test the synchronous kernel log sink's byte-count contract."""

from pathlib import Path
import os
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"
COMPILER = os.environ.get("CC", "cc")


def main():
    with tempfile.TemporaryDirectory(prefix="nos-log-sink-test-") as tmp:
        exe = Path(tmp) / "test_log_sink"
        subprocess.run(
            [COMPILER, "-std=gnu11", "-O1", "-DCONFIG_LOG_FIFO_BUF_SIZE=512",
             "-I", str(TEST_DIR / "stubs"), "-I", str(ROOT / "include"),
             str(ROOT / "kernel/printk/log.c"), str(ROOT / "kernel/printk/console_backend.c"), str(TEST_DIR / "test_log_sink.c"),
             "-o", str(exe)], check=True,
        )
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
