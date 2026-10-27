#!/usr/bin/env python3
"""Exercise IRQ-state restoration under two-CPU lock contention."""

from pathlib import Path
import os
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"


def main():
    with tempfile.TemporaryDirectory(prefix="nos-spinlock-test-") as tmp:
        exe = Path(tmp) / "test_spinlock_irq"
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-pthread",
             "-I", str(TEST_DIR / "stubs"), "-I", str(ROOT / "include"),
             str(TEST_DIR / "test_spinlock_irq.c"), "-o", str(exe)], check=True,
        )
        subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == "__main__":
    main()
