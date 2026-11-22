#!/usr/bin/env python3
"""Exercise AI-Native VMO/TensorObj sealing, W^X, CoW split, KV-prefix share and quota rollback."""

from pathlib import Path
import os
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"


def main():
    with tempfile.TemporaryDirectory(prefix="nos-vmo-test-") as tmp:
        exe = Path(tmp) / "test_vmo"
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror",
             "-D__HOST_TEST__=1", "-DCONFIG_NOS_CAP_CDT=1", "-DCONFIG_NOS_AI_CORE=1",
             "-I", str(TEST_DIR / "stubs"), "-I", str(ROOT / "include"),
             str(TEST_DIR / "test_vmo.c"),
             str(ROOT / "kernel" / "ipc" / "object.c"),
             str(ROOT / "kernel" / "mm" / "vmo.c"),
             "-o", str(exe)], check=True,
        )
        subprocess.run([str(exe)], check=True, timeout=15)


if __name__ == "__main__":
    main()
