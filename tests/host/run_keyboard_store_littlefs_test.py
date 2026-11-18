#!/usr/bin/env python3
# Copyright (C) 2026 Nick Hu <huqihan@live.com>
# Author: Nick Hu
"""Exercise keyboard transactions through production NOS VFS and LittleFS."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
littlefs = root / "libs/third_party/littlefs"
with tempfile.TemporaryDirectory(prefix="nos-keyboard-littlefs-") as tmp:
    exe = Path(tmp) / "test"
    gc = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-O1",
                    "-ffunction-sections", "-fdata-sections", "-Wno-pointer-sign", gc,
                    "-I", str(root / "tests/host/stubs"), "-I", str(root / "include"),
                    "-I", str(littlefs), str(littlefs / "lfs.c"),
                    str(littlefs / "lfs_util.c"),
                    str(root / "app/server/keyboard/keyboard_object_store.c"),
                    str(root / "tests/host/test_keyboard_store_littlefs.c"),
                    "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
