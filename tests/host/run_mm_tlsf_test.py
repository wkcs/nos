#!/usr/bin/env python3
"""Exercise the TLSF O(1) low-fragmentation allocator."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="nos-mm-tlsf-") as tmp:
    exe = Path(tmp) / "test"
    subprocess.run(
        [
            os.environ.get("CC", "cc"),
            "-std=gnu11",
            "-pthread",
            "-O2",
            "-Wall",
            "-Wextra",
            "-I",
            str(root / "tests/host/mm_block_stubs"),
            "-I",
            str(root / "include"),
            str(root / "kernel/mm/mm_tlsf.c"),
            str(root / "tests/host/test_mm_tlsf.c"),
            "-o",
            str(exe),
        ],
        check=True,
    )
    subprocess.run([str(exe)], check=True, timeout=30)
