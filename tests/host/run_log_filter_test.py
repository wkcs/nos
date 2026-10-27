#!/usr/bin/env python3
"""Check compile-time log filtering and argument evaluation."""

from pathlib import Path
import os
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"


def main():
    with tempfile.TemporaryDirectory(prefix="nos-log-filter-test-") as tmp:
        exe = Path(tmp) / "test_log_filter"
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=gnu11", "-O2",
             "-DCONFIG_LOG_COMPILE_LEVEL=3", "-I", str(TEST_DIR / "stubs"),
             "-I", str(ROOT / "include"), str(TEST_DIR / "test_log_filter.c"),
             "-o", str(exe)], check=True,
        )
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
