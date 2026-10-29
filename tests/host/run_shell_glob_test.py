#!/usr/bin/env python3
"""Test shell pathname pattern matching and quoting."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="nos-shell-glob-") as tmp:
    exe = Path(tmp) / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-Wall", "-Wextra", "-Werror",
                    str(root / "app/shell/glob.c"), str(root / "tests/host/test_shell_glob.c"),
                    "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
