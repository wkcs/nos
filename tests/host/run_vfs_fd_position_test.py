#!/usr/bin/env python3
"""Verify operations sharing a descriptor serialize their file position."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="nos-vfs-position-") as tmp:
    exe = Path(tmp) / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra",
                    "-Werror", "-pthread", "-I", str(root / "tests/host/vfs_fd_stubs"),
                    str(root / "fs/vfs_fd.c"), str(root / "fs/vfs_io.c"),
                    str(root / "tests/host/test_vfs_fd_position.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
