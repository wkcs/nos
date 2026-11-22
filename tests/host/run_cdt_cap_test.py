#!/usr/bin/env python3
"""Exercise expandable 4096-slot CNode and CDT revocable capability tree."""

from pathlib import Path
import os
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"


def main():
    with tempfile.TemporaryDirectory(prefix="nos-cdt-cap-test-") as tmp:
        exe = Path(tmp) / "test_cdt_cap"
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror",
             "-DCONFIG_NOS_CAP_CDT=1", "-DCONFIG_NOS_CAP_MAX_SLOTS=16",
             "-I", str(TEST_DIR / "stubs"), "-I", str(ROOT / "include"),
             str(TEST_DIR / "test_cdt_cap.c"),
             str(ROOT / "kernel" / "ipc" / "object.c"),
             str(ROOT / "kernel" / "cap" / "cap_table.c"),
             "-o", str(exe)], check=True,
        )
        subprocess.run([str(exe)], check=True, timeout=15)


if __name__ == "__main__":
    main()
