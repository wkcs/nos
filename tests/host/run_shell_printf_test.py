#!/usr/bin/env python3
"""Test shell printf output, escape decoding, and failures."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="nos-shell-printf-") as tmp:
    exe = Path(tmp) / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-Wall", "-Wextra", "-Werror",
                    str(root / "app/shell/printf_cmd.c"),
                    str(root / "tests/host/test_shell_printf.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
