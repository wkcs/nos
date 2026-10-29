#!/usr/bin/env python3
"""Exercise the real memory block allocator with concurrent callers."""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="nos-mm-block-") as tmp:
    exe = Path(tmp) / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-pthread", "-O1",
                    "-I", str(root / "tests/host/mm_block_stubs"),
                    str(root / "kernel/mm/mm_block.c"), str(root / "kernel/mm/mm.c"),
                    str(root / "tests/host/test_mm_block.c"),
                    "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=30)
