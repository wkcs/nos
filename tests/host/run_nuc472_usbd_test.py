#!/usr/bin/env python3
"""运行 NUC472 USBD 全速控制器驱动与状态机主机回归测试。"""

from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"
CC = "clang" if subprocess.run(["which", "clang"], capture_output=True).returncode == 0 else "gcc"


def main():
    with tempfile.TemporaryDirectory(prefix="nos-nuc472-usbd-test-") as tmp:
        exe = Path(tmp) / "test_nuc472_usbd"
        cmd = [
            CC,
            "-Wall",
            "-Wextra",
            "-Werror",
            "-std=gnu11",
            "-I",
            str(TEST_DIR / "stubs"),
            "-I",
            str(TEST_DIR / "shell_pipe_stubs"),
            "-I",
            str(ROOT / "include"),
            "-I",
            str(ROOT / "arch/arm/include"),
            "-I",
            str(ROOT / "soc/nuc472/include"),
            str(TEST_DIR / "test_nuc472_usbd.c"),
            "-lpthread",
            "-o",
            str(exe),
        ]
        subprocess.run(cmd, check=True)
        subprocess.run([str(exe)], check=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
