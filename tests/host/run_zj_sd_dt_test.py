#!/usr/bin/env python3
"""Check the actual ZJ-V3 SD device tree against the shared SPI resolver."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / 'tests' / 'host'
cc = os.environ.get('CC', 'cc')
with tempfile.TemporaryDirectory(prefix='nos-zj-sd-dt-test-') as tmp:
    path = Path(tmp)
    subprocess.run([cc, '-E', '-P', '-x', 'assembler-with-cpp', '-undef', '-nostdinc',
                    '-I', str(ROOT / 'dts'), str(ROOT / 'board/zj-v3/board.dts'),
                    '-o', str(path / 'board.dts')], check=True)
    subprocess.run([sys.executable, str(ROOT / 'scripts/gen_devicetree.py'), '--input',
                    str(path / 'board.dts'), '--output', str(path / 'tree.c'), '--bindings',
                    str(ROOT / 'dts/bindings')], check=True)
    subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(TEST_DIR / 'stubs'), '-I', str(ROOT / 'include'),
                    str(TEST_DIR / 'test_zj_sd_dt.c'), str(path / 'tree.c'),
                    str(ROOT / 'drivers/base/devicetree.c'), str(ROOT / 'drivers/spi/spi_dt.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
