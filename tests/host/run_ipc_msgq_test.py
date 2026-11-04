#!/usr/bin/env python3
"""Exercise bounded fixed-element message queue nos_msgq integrated with wait foundation."""

from pathlib import Path
import os
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"


def main():
    with tempfile.TemporaryDirectory(prefix="nos-ipc-msgq-test-") as tmp:
        exe = Path(tmp) / "test_ipc_msgq"
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=gnu11", "-O1",
             "-I", str(TEST_DIR / "stubs"), "-I", str(ROOT / "include"),
             str(TEST_DIR / "test_ipc_msgq.c"),
             str(ROOT / "kernel" / "ipc" / "timeout.c"),
             str(ROOT / "kernel" / "ipc" / "object.c"),
             str(ROOT / "kernel" / "ipc" / "wait.c"),
             str(ROOT / "kernel" / "ipc" / "msgq.c"),
             "-lpthread",
             "-o", str(exe)], check=True,
        )
        subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == "__main__":
    main()
