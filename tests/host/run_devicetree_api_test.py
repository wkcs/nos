#!/usr/bin/env python3
"""Test device-tree lookup and matching with a static synthetic tree."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / 'tests' / 'host'
with tempfile.TemporaryDirectory(prefix='nos-devicetree-test-') as tmp:
    binary = Path(tmp) / 'test_devicetree_api'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', '-I', str(TEST_DIR / 'stubs'), '-I', str(ROOT / 'include'),
                    str(TEST_DIR / 'test_devicetree_api.c'), str(ROOT / 'drivers/base/devicetree.c'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
