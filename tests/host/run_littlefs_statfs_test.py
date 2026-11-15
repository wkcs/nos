#!/usr/bin/env python3
"""Verify LittleFS space reports follow block allocations on a flash image."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
littlefs = root / "libs/third_party/littlefs"
with tempfile.TemporaryDirectory(prefix="nos-littlefs-statfs-") as tmp:
    exe = Path(tmp) / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-O1",
                    "-I", str(root / "tests/host/stubs"), "-I", str(root / "include"),
                    "-I", str(littlefs), str(littlefs / "lfs.c"),
                    str(littlefs / "lfs_util.c"),
                    str(root / "tests/host/test_littlefs_statfs.c"),
                    "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
