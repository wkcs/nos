#!/usr/bin/env python3
"""Check stack alignment and allocation bounds across address widths."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="nos-task-stack-") as tmp:
    exe = Path(tmp) / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-Wall", "-Wextra",
                    "-Werror", str(root / "tests/host/test_task_stack_layout.c"),
                    "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
