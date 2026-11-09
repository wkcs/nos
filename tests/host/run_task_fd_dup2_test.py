#!/usr/bin/env python3
"""Run per-task file descriptor and dup/dup2 unit tests."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"


def run_test():
    with tempfile.TemporaryDirectory(prefix="nos-task-fd-dup2-") as tmp:
        exe = Path(tmp) / "test_task_fd_dup2"
        sources = [
            str(ROOT / "fs" / "vfs_fd.c"),
            str(TEST_DIR / "test_task_fd_dup2.c"),
        ]
        subprocess.run(
            [
                os.environ.get("CC", "cc"),
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pthread",
                "-DTEST_CUSTOM_TASK_CURRENT",
                "-I",
                str(TEST_DIR / "vfs_fd_stubs"),
                *sources,
                "-o",
                str(exe),
            ],
            check=True,
        )
        subprocess.run([str(exe)], check=True)


class TaskFdDup2Test(unittest.TestCase):
    def test_task_fd_dup2(self):
        run_test()


def main():
    run_test()


if __name__ == "__main__":
    main()
