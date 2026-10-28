#!/usr/bin/env python3
"""Test GPIO I2C protocol against a simulated slave and clock line."""
from pathlib import Path
import os
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / 'tests' / 'host'
with tempfile.TemporaryDirectory(prefix='nos-i2c-gpio-test-') as tmp:
    exe = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(TEST_DIR / 'stm32_spi_stubs'), '-I', str(TEST_DIR / 'stubs'),
                    '-I', str(ROOT / 'include'), str(TEST_DIR / 'test_i2c_gpio.c'),
                    str(ROOT / 'drivers/i2c/i2c_gpio.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
