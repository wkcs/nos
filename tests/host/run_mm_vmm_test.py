#!/usr/bin/env python3
"""Exercise MMU multi-level page tables, VMA intervals, demand paging, and Copy-on-Write."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="nos-mm-vmm-") as tmp:
    exe = Path(tmp) / "test"
    subprocess.run(
        [
            os.environ.get("CC", "cc"),
            "-std=gnu11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-I",
            str(root / "tests/host/mm_block_stubs"),
            "-I",
            str(root / "include"),
            str(root / "kernel/mm/vmm_mmu.c"),
            str(root / "tests/host/test_mm_vmm.c"),
            "-o",
            str(exe),
        ],
        check=True,
    )
    subprocess.run([str(exe)], check=True, timeout=30)
