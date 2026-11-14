#!/usr/bin/env python3
"""nShell 扩展命令注册与查找机制主机回归测试。"""

from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    with tempfile.TemporaryDirectory(prefix="nos-shell-ext-cmd-") as tmp:
        exe = Path(tmp) / "test_shell_ext_cmd"
        srcs = [
            ROOT / "app/shell/nshell.c",
            ROOT / "tests/host/test_shell_ext_cmd.c",
        ]
        cmd = [
            os.environ.get("CC", "cc"),
            "-std=gnu11",
            "-O1",
            "-Wall",
            "-Wextra",
            "-DSHELL_EXT_CMD_HOST_TEST",
            "-I", str(ROOT / "include"),
            "-I", str(ROOT / "tests/host/stubs"),
            "-I", str(ROOT / "app/shell"),
            *([str(s) for s in srcs]),
            "-o", str(exe),
        ]
        subprocess.run(cmd, check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
