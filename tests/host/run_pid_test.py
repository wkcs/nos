#!/usr/bin/env python3
"""Exercise PID interval reuse and coalescing."""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="nos-pid-") as tmp:
    exe = Path(tmp) / "test"
    sanitizer = ["-fsanitize=address"] if os.environ.get("NOS_TEST_ASAN") == "1" else []
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    *sanitizer, "-I", str(root / "tests/host/pid_stubs"),
                    str(root / "kernel/pid.c"), str(root / "tests/host/test_pid.c"),
                    "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=15)
