#!/usr/bin/env python3
"""Resolve flash, SPI and GPIO resources from a simulated device tree."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / 'tests' / 'host'
with tempfile.TemporaryDirectory(prefix='nos-w25q-dt-test-') as tmp:
    binary = Path(tmp) / 'test_w25q_dt'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', '-I', str(TEST_DIR / 'stubs'), '-I', str(ROOT / 'include'),
                    str(TEST_DIR / 'test_w25q_dt.c'), str(ROOT / 'drivers/flash/w25q_dt.c'),
                    str(ROOT / 'drivers/base/devicetree.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
