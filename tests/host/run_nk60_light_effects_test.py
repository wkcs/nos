#!/usr/bin/env python3
"""Compile and run the NK60 lighting renderer regression."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="nk60-light-test-") as tmp:
    exe = Path(tmp) / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra",
                    "-I", str(root / "tests/host/stubs"), "-I", str(root / "include"),
                    str(root / "tests/host/test_nk60_light_effects.c"), "-o", str(exe)],
                   check=True)
    subprocess.run([str(exe)], check=True)
