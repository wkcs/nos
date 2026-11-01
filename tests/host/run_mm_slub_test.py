#!/usr/bin/env python3
"""Exercise the SLUB kmem_cache small-object allocator and polymorphic mm_node."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="nos-mm-slub-") as tmp:
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
            str(root / "kernel/mm/mm_slub.c"),
            str(root / "tests/host/test_mm_slub.c"),
            "-o",
            str(exe),
        ],
        check=True,
    )
    subprocess.run([str(exe)], check=True, timeout=30)
