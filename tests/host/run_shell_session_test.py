#!/usr/bin/env python3
"""Exercise independent shell state and terminal routing."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="nos-shell-session-") as tmp:
    exe = Path(tmp) / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra",
                    "-I", str(ROOT / "tests/host/stubs"), "-I", str(ROOT / "include"),
                    *([str(ROOT / "app/shell/session.c")] if (ROOT / "app/shell/session.c").exists() else []), str(ROOT / "kernel/tty/tty.c"),
                    str(ROOT / "tests/host/test_shell_session.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
