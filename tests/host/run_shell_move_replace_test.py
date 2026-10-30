#!/usr/bin/env python3
"""Exercise replacement and recovery under injected filesystem failures."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="nos-shell-move-") as tmp:
    exe = Path(tmp) / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-I", str(root / "tests/host/move_replace_stubs"),
                    str(root / "app/shell/move_replace.c"),
                    str(root / "tests/host/test_shell_move_replace.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
