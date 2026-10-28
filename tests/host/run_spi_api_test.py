#!/usr/bin/env python3
"""Exercise SPI transaction validation and controller dispatch on the host."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / 'tests' / 'host'

with tempfile.TemporaryDirectory(prefix='nos-spi-test-') as tmp:
    binary = Path(tmp) / 'test_spi_api'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', '-I', str(TEST_DIR / 'stubs'), '-I', str(ROOT / 'include'),
                    str(TEST_DIR / 'test_spi_api.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
