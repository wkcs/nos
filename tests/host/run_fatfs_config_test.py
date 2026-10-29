#!/usr/bin/env python3
"""Check the memory and locking properties of the FatFs configuration."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="nos-fatfs-config-") as tmp:
    exe = Path(tmp) / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-Wall", "-Wextra", "-Werror",
                    str(root / "tests/host/test_fatfs_config.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
