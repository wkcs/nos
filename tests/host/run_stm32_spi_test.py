#!/usr/bin/env python3
"""Test STM32 SPI transactions with a simulated peripheral."""
from pathlib import Path
import os
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / 'tests' / 'host'
with tempfile.TemporaryDirectory(prefix='nos-stm32-spi-test-') as tmp:
    exe = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-DCONFIG_STM32_SPI_POLL_TIMEOUT=3', '-I', str(TEST_DIR / 'stm32_spi_stubs'), '-I', str(TEST_DIR / 'stubs'),
                    '-I', str(ROOT / 'include'), str(TEST_DIR / 'test_stm32_spi.c'),
                    str(ROOT / 'drivers/spi/spi_stm32f4.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
