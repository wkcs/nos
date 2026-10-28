#!/usr/bin/env python3
"""Keep in-flight descriptor operations alive across concurrent close."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="nos-vfs-fd-") as tmp:
    exe = Path(tmp) / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-Wall", "-Wextra",
                    "-Werror", "-pthread", "-I", str(root / "tests/host/vfs_fd_stubs"),
                    str(root / "fs/vfs_fd.c"), str(root / "tests/host/test_vfs_fd_lifetime.c"),
                    "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
