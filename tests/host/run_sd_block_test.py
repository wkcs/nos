#!/usr/bin/env python3
"""Test block adapter bounds, serialization, and error propagation."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / 'tests' / 'host'
with tempfile.TemporaryDirectory(prefix='nos-sd-block-test-') as tmp:
    binary = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', '-I', str(TEST_DIR / 'stm32_spi_stubs'),
                    '-I', str(TEST_DIR / 'stubs'), '-I', str(ROOT / 'include'),
                    str(TEST_DIR / 'test_sd_block.c'), str(ROOT / 'drivers/block/sd_block.c'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
