#!/usr/bin/env python3
"""Run raw TTY contract tests against the production implementation."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"


def run_test():
    with tempfile.TemporaryDirectory(prefix="nos-tty-test-") as tmp:
        exe = Path(tmp) / "test_tty"
        tty = ROOT / "kernel/tty/tty.c"
        sources = [str(TEST_DIR / "test_tty.c")]
        if tty.exists():
            sources.append(str(tty))
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=gnu11",
             "-I", str(TEST_DIR / "stubs"), "-I", str(ROOT / "include"),
             *sources, "-o", str(exe)], check=True,
        )
        subprocess.run([str(exe)], check=True)


class TtyTest(unittest.TestCase):
    def test_tty(self):
        run_test()


def main():
    run_test()


if __name__ == "__main__":
    main()
