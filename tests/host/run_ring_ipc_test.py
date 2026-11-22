#!/usr/bin/env python3
"""Exercise asynchronous zero-copy RingIPC, Schema whitelist, Cell budgets and Deadline Inheritance."""

from pathlib import Path
import os
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"


def main():
    with tempfile.TemporaryDirectory(prefix="nos-ring-ipc-test-") as tmp:
        exe = Path(tmp) / "test_ring_ipc"
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror",
             "-D__HOST_TEST__=1", "-DCONFIG_NOS_CAP_CDT=1",
             "-DCONFIG_NOS_AI_CORE=1", "-DCONFIG_NOS_CAP_MAX_SLOTS=32",
             "-I", str(TEST_DIR / "stubs"), "-I", str(ROOT / "include"),
             str(TEST_DIR / "test_ring_ipc.c"),
             str(ROOT / "kernel" / "ipc" / "object.c"),
             str(ROOT / "kernel" / "cap" / "cap_table.c"),
             str(ROOT / "kernel" / "mm" / "vmo.c"),
             str(ROOT / "kernel" / "ai" / "cell.c"),
             str(ROOT / "kernel" / "ipc" / "ring_ipc.c"),
             "-lpthread",
             "-o", str(exe)], check=True,
        )
        subprocess.run([str(exe)], check=True, timeout=15)


if __name__ == "__main__":
    main()
