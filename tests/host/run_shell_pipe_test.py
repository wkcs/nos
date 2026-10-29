#!/usr/bin/env python3
"""Exercise shell stream backpressure, EOF, and endpoint closure."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="nos-shell-pipe-") as tmp:
    exe = Path(tmp) / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-pthread", "-I", str(root / "tests/host/shell_pipe_stubs"),
                    str(root / "app/shell/pipe.c"), str(root / "tests/host/test_shell_pipe.c"),
                    "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=15)
