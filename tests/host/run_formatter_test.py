#!/usr/bin/env python3
"""Compile the actual kernel formatter against minimal host headers and test it."""

from pathlib import Path
import os
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"
COMPILER = os.environ.get("CC", "cc")


def main():
    with tempfile.TemporaryDirectory(prefix="nos-format-test-") as tmp:
        obj = Path(tmp) / "vsprintf.o"
        exe = Path(tmp) / "test_vsprintf"
        definitions = [
            "-Dvsnprintf=nos_vsnprintf",
            "-Dsnprintf=nos_snprintf",
            "-Dvsprintf=nos_vsprintf",
            "-Dsprintf=nos_sprintf",
        ]
        subprocess.run(
            [COMPILER, "-std=gnu11", "-fgnu89-inline", "-O1", "-fno-builtin",
             "-fsanitize=address", *definitions, "-I", str(TEST_DIR / "stubs"),
             "-c", str(ROOT / "libs" / "vsprintf.c"), "-o", str(obj)],
            check=True,
        )
        subprocess.run(
            [COMPILER, "-std=gnu11", "-O1", "-fno-builtin", "-fsanitize=address",
             str(TEST_DIR / "test_vsprintf.c"), str(obj), "-o", str(exe)],
            check=True,
        )
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
