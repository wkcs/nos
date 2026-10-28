#!/usr/bin/env python3
"""Test SD SPI block I/O with a simulated SD card."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / 'tests' / 'host'
with tempfile.TemporaryDirectory(prefix='nos-sd-io-test-') as tmp:
    binary = Path(tmp) / 'test_sd_spi_io'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', '-I', str(TEST_DIR / 'stubs'), '-I', str(ROOT / 'include'),
                    str(TEST_DIR / 'test_sd_spi_io.c'), str(ROOT / 'drivers/block/sd_spi.c'),
                    str(ROOT / 'drivers/block/sd_spi_io.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
