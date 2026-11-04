#!/usr/bin/env python3
"""Exercise IPC universal wait queue, wait entry state machine, and deferred wake batch."""

from pathlib import Path
import os
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"


def main():
    with tempfile.TemporaryDirectory(prefix="nos-ipc-wait-test-") as tmp:
        exe = Path(tmp) / "test_ipc_wait"
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=gnu11", "-O1",
             "-I", str(TEST_DIR / "stubs"), "-I", str(ROOT / "include"),
             str(TEST_DIR / "test_ipc_wait.c"),
             str(ROOT / "kernel" / "ipc" / "timeout.c"),
             str(ROOT / "kernel" / "ipc" / "object.c"),
             str(ROOT / "kernel" / "ipc" / "wait.c"),
             "-o", str(exe)], check=True,
        )
        subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == "__main__":
    main()
