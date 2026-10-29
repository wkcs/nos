#!/usr/bin/env python3
"""Test buddy page allocation and free-list accounting."""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="nos-buddy-") as tmp:
    exe = Path(tmp) / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-pthread", "-I",
                    str(root / "tests/host/buddy_stubs"), str(root / "kernel/mm/mm_buddy.c"),
                    str(root / "tests/host/test_mm_buddy.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)
