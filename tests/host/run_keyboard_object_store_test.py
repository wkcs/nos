#!/usr/bin/env python3
# Copyright (C) 2026 Nick Hu <huqihan@live.com>
# Author: Nick Hu
"""Compile and run the transactional object store against a real host filesystem."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'store_test'
    subprocess.run(['cc', '-std=c11', '-D_DEFAULT_SOURCE', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root / 'tests/host/keyboard_store_stubs'),
                    '-I' + str(root / 'include'),
                    str(root / 'tests/host/keyboard_object_store_test.c'),
                    str(root / 'app/server/keyboard/keyboard_object_store.c'),
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe), tmp], check=True)
