#!/usr/bin/env python3
"""Test shell quoting and rejection of unsupported syntax."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="nos-shell-parse-") as tmp:
    exe = Path(tmp) / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-Wall", "-Wextra",
                    "-Werror", str(root / "app/shell/parse.c"),
                    str(root / "tests/host/test_shell_parse.c"), "-o", str(exe)],
                   check=True)
    subprocess.run([str(exe)], check=True)
