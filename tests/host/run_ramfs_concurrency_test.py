#!/usr/bin/env python3
"""Exercise actual RAMFS directory operations with contending host threads."""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="nos-ramfs-") as tmp:
    exe = Path(tmp) / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-pthread", "-O1",
                    "-I", str(root / "tests/host/ramfs_stubs"),
                    str(root / "tests/host/test_ramfs_concurrency.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=15)
