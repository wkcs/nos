#!/usr/bin/env python3
"""Test SD SPI initialization with a simulated SD card."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / 'tests' / 'host'
with tempfile.TemporaryDirectory(prefix='nos-sd-init-test-') as tmp:
    binary = Path(tmp) / 'test_sd_spi_init'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', '-I', str(TEST_DIR / 'sd_spi_stubs'),
                    '-I', str(TEST_DIR / 'stubs'), '-I', str(ROOT / 'include'),
                    str(TEST_DIR / 'test_sd_spi_init.c'), str(ROOT / 'drivers/block/sd_spi.c'),
                    str(ROOT / 'drivers/block/sd_spi_card.c'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
