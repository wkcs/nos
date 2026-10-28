#!/usr/bin/env python3
"""Test flash sector preservation and error paths against a simulated NOR chip."""
from pathlib import Path
import os
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / 'tests' / 'host'
with tempfile.TemporaryDirectory(prefix='nos-w25q-flash-test-') as tmp:
    exe = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(TEST_DIR / 'w25q_stubs'), '-I', str(TEST_DIR / 'stm32_spi_stubs'),
                    '-I', str(TEST_DIR / 'stubs'), '-I', str(ROOT / 'include'),
                    str(TEST_DIR / 'test_w25q_flash.c'), str(ROOT / 'drivers/flash/w25q_flash.c'),
                    str(ROOT / 'drivers/flash/w25q_spi.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
