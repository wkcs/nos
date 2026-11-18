#!/usr/bin/env python3
# Copyright (C) 2026 Nick Hu <huqihan@live.com>
# Author: Nick Hu
"""Check production NLK VFS guards against managed-path spelling bypasses."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="nos-cap-vfs-managed-") as tmp:
    exe = Path(tmp) / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-O1",
                    "-Wall", "-Wextra", "-Werror", "-DCONFIG_VFS",
                    "-I", str(root / "tests/host/stubs"), "-I", str(root / "include"),
                    str(root / "app/server/nlk/caps/cap_vfs.c"),
                    str(root / "app/server/nlk/caps/vfs_path_policy.c"),
                    str(root / "tests/host/test_cap_vfs_managed_paths.c"),
                    "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
