#!/usr/bin/env python3
"""Exercise the I2C controller transaction interface on the host."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / 'tests' / 'host'
with tempfile.TemporaryDirectory(prefix='nos-i2c-test-') as tmp:
    binary = Path(tmp) / 'test_i2c_api'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', '-I', str(TEST_DIR / 'stubs'), '-I', str(ROOT / 'include'),
                    str(TEST_DIR / 'test_i2c_api.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
