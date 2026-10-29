#!/usr/bin/env python3
"""Exercise basename and dirname path handling."""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="nos-shell-path-") as tmp:
    exe = Path(tmp) / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-Wall", "-Wextra", "-Werror",
                    str(root / "app/shell/path.c"), str(root / "tests/host/test_shell_path.c"),
                    "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
