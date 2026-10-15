#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Foreground guest launcher."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
from qemu_platforms import PLATFORMS, qemu_command

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--platform', choices=PLATFORMS, required=True)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--qemu-bindir', default=os.environ.get('QEMU_BINDIR', ''))
    parser.add_argument('--cpus', type=int, choices=range(1, 5), default=1)
    parser.add_argument('--disk', type=Path)
    args = parser.parse_args()
    return subprocess.run(qemu_command(args.platform, args.image, args.qemu_bindir, args.cpus, args.disk)).returncode

if __name__ == '__main__':
    sys.exit(main())
