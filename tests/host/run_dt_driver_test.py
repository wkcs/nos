#!/usr/bin/env python3
"""Test static device-tree driver matching, probe and retry."""
from pathlib import Path
import os
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / 'tests' / 'host'
with tempfile.TemporaryDirectory(prefix='nos-dt-driver-test-') as tmp:
    exe = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(TEST_DIR / 'stm32_spi_stubs'), '-I', str(TEST_DIR / 'stubs'),
                    '-I', str(ROOT / 'include'), str(TEST_DIR / 'test_dt_driver.c'),
                    str(ROOT / 'drivers/base/dt_driver.c'), str(ROOT / 'drivers/base/devicetree.c'),
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
