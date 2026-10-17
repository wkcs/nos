#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Real 256-byte path and bulk IPC regression."""
from qemu_glibc import main

if __name__ == '__main__':
    raise SystemExit(main('posix-long-path'))
