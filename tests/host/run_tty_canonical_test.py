#!/usr/bin/env python3
"""Exercise the optional canonical TTY against a fake byte stream."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"


def run_test():
    with tempfile.TemporaryDirectory(prefix="nos-tty-canonical-test-") as tmp:
        exe = Path(tmp) / "test_tty_canonical"
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=gnu11", "-DCONFIG_TTY_CANONICAL",
             "-DCONFIG_TTY_LINE_SIZE=8", "-I", str(TEST_DIR / "stubs"),
             "-I", str(ROOT / "include"), str(TEST_DIR / "test_tty_canonical.c"),
             str(ROOT / "kernel/tty/tty.c"), "-o", str(exe)], check=True,
        )
        subprocess.run([str(exe)], check=True)


class TtyCanonicalTest(unittest.TestCase):
    def test_tty_canonical(self):
        run_test()


def main():
    run_test()


if __name__ == "__main__":
    main()
