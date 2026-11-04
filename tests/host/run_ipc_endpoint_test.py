#!/usr/bin/env python3
"""Exercise microkernel synchronous endpoint IPC with in-flight reply queue and pinned capability lookup."""

from pathlib import Path
import os
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"


def main():
    with tempfile.TemporaryDirectory(prefix="nos-ipc-endpoint-test-") as tmp:
        exe = Path(tmp) / "test_ipc_endpoint"
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=gnu11", "-O1",
             "-I", str(TEST_DIR / "stubs"), "-I", str(ROOT / "include"),
             str(TEST_DIR / "test_ipc_endpoint.c"),
             str(ROOT / "kernel" / "ipc" / "timeout.c"),
             str(ROOT / "kernel" / "ipc" / "object.c"),
             str(ROOT / "kernel" / "ipc" / "wait.c"),
             str(ROOT / "kernel" / "ipc" / "endpoint.c"),
             str(ROOT / "kernel" / "cap" / "cap_table.c"),
             "-lpthread",
             "-o", str(exe)], check=True,
        )
        subprocess.run([str(exe)], check=True, timeout=15)


if __name__ == "__main__":
    main()
