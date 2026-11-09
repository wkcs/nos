#!/usr/bin/env python3
"""Run lightweight DevFS device registration and contract tests."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"


def run_test():
    with tempfile.TemporaryDirectory(prefix="nos-devfs-test-") as tmp:
        exe = Path(tmp) / "test_devfs"
        sources = [
            str(TEST_DIR / "test_devfs.c"),
            str(ROOT / "fs" / "devfs.c"),
            str(ROOT / "fs" / "vfs_fd.c"),
            str(ROOT / "fs" / "vfs_io.c"),
            str(ROOT / "kernel" / "tty" / "tty.c"),
        ]
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=gnu11",
             "-I", str(TEST_DIR / "stubs"), "-I", str(ROOT / "include"),
             *sources, "-o", str(exe)], check=True,
        )
        subprocess.run([str(exe)], check=True)


class DevfsTest(unittest.TestCase):
    def test_devfs(self):
        run_test()


def main():
    run_test()


if __name__ == "__main__":
    main()
