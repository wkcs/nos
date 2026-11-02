#!/usr/bin/env python3
"""Run Block Cache regression tests."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / 'tests' / 'host'
with tempfile.TemporaryDirectory(prefix='nos-block-cache-test-') as tmp:
    binary = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', '-I', str(TEST_DIR / 'stubs'), '-I', str(ROOT / 'include'),
                    str(TEST_DIR / 'test_block_cache.c'), str(ROOT / 'drivers/block/block_cache.c'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
