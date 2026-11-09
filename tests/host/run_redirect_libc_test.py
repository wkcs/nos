#!/usr/bin/env python3
"""Run libc standard stream redirection unit tests."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"


def run_test():
    with tempfile.TemporaryDirectory(prefix="nos-redirect-libc-") as tmp:
        exe = Path(tmp) / "test_redirect_libc"
        sources = [
            str(ROOT / "fs" / "vfs_fd.c"),
            str(ROOT / "fs" / "vfs_io.c"),
            str(ROOT / "libs" / "libc" / "port" / "picolibc_newlib.c"),
            str(TEST_DIR / "test_redirect_libc.c"),
        ]
        subprocess.run(
            [
                os.environ.get("CC", "cc"),
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-pthread",
                "-DCONFIG_FS",
                "-D__HOST_TEST__",
                "-DTEST_CUSTOM_TASK_CURRENT",
                "-I",
                str(TEST_DIR / "redirect_libc_stubs"),
                *sources,
                "-o",
                str(exe),
            ],
            check=True,
        )
        subprocess.run([str(exe)], check=True)


class RedirectLibcTest(unittest.TestCase):
    def test_redirect_libc(self):
        run_test()


def main():
    run_test()


if __name__ == "__main__":
    main()
