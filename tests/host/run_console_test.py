#!/usr/bin/env python3
"""Test console errors and byte semantics on a host."""

from pathlib import Path
import os
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"


def main():
    with tempfile.TemporaryDirectory(prefix="nos-console-test-") as tmp:
        exe = Path(tmp) / "test_console"
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=gnu11", "-Wno-incompatible-function-pointer-types",
             "-I", str(TEST_DIR / "stubs"), "-I", str(ROOT / "include"),
             str(TEST_DIR / "test_console.c"), "-o", str(exe)], check=True,
        )
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
