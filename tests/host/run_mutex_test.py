#!/usr/bin/env python3
"""Exercise the real mutex handoff at the unlock/enqueue boundary."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="nos-mutex-") as tmp:
    exe = Path(tmp) / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-I",
                    str(root / "tests/host/mutex_stubs"),
                    str(root / "kernel/ipc/mutex.c"), str(root / "tests/host/test_mutex.c"),
                    "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)
