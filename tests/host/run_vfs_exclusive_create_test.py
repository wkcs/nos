#!/usr/bin/env python3
"""Check that removed persistent-filesystem inodes obey dentry lifetimes."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="nos-vfs-exclusive-") as tmp:
    exe = Path(tmp) / "test"
    gc = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-O1",
                    "-ffunction-sections", "-fdata-sections", "-Wno-pointer-sign", gc,
                    "-I", str(root / "tests/host/stubs"), "-I", str(root / "include"),
                    str(root / "tests/host/test_vfs_exclusive_create.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
