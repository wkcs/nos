#!/usr/bin/env python3
"""Run kernel printk against the real formatter and a captured output backend."""

from pathlib import Path
import os
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"
COMPILER = os.environ.get("CC", "cc")
COMMON = ["-std=gnu11", "-fgnu89-inline", "-O1", "-fno-builtin",
          "-fsanitize=address", "-DCONFIG_LOG_MESSAGE_SIZE=128",
          "-I", str(TEST_DIR / "stubs"), "-I", str(ROOT / "include")]
RENAMES = ["-Dvsnprintf=nos_vsnprintf", "-Dsnprintf=nos_snprintf",
           "-Dvsprintf=nos_vsprintf", "-Dsprintf=nos_sprintf"]


def main():
    with tempfile.TemporaryDirectory(prefix="nos-printk-test-") as tmp:
        obj_format = Path(tmp) / "vsprintf.o"
        obj_printk = Path(tmp) / "printk.o"
        exe = Path(tmp) / "test_printk"
        for source, obj in [(ROOT / "libs/vsprintf.c", obj_format),
                            (ROOT / "kernel/printk/printk.c", obj_printk)]:
            subprocess.run([COMPILER, *COMMON, *RENAMES, "-c", str(source),
                            "-o", str(obj)], check=True)
        subprocess.run([COMPILER, *COMMON, str(TEST_DIR / "test_printk.c"),
                        str(obj_format), str(obj_printk), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
