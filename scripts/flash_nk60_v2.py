#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Validate and program NK60 V2 via SWD.

The default prints a validated plan. --execute explicitly contacts ST-Link.
No bootloader, option bytes, mass erase, or external flash operation is used.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FLASH_START = 0x08000000
FLASH_LIMIT = FLASH_START + 128 * 1024


def validate_image(path):
    data = path.read_bytes()
    if len(data) < 52 or data[:7] != b'\x7fELF\x01\x01\x01':
        raise ValueError('expected a little-endian ELF32 image')
    kind, machine, version = struct.unpack_from('<HHI', data, 16)
    entry, headers = struct.unpack_from('<II', data, 24)
    header_size, count = struct.unpack_from('<HH', data, 42)
    if (kind, machine, version) != (2, 40, 1) or header_size != 32 or not count:
        raise ValueError('expected an ARM executable with ELF32 program headers')
    if count > 32 or headers < 52 or headers + count * header_size > len(data):
        raise ValueError('invalid program header table')
    if not entry & 1 or not FLASH_START <= entry - 1 < FLASH_LIMIT:
        raise ValueError('entry must be Thumb code in the NK60 application region')
    spans = []
    vector = None
    executable_entry = False
    for index in range(count):
        ptype, offset, virtual, physical, size, memory, flags, _ = struct.unpack_from(
            '<IIIIIIII', data, headers + index * header_size)
        if ptype != 1:
            continue
        if memory < size or offset + size > len(data):
            raise ValueError('truncated or inconsistent load segment')
        if not size:
            continue
        if physical < FLASH_START or physical + size > FLASH_LIMIT:
            raise ValueError('load segment writes outside the 128 KiB erase-aligned application partition')
        if any(physical < end and start < physical + size for start, end in spans):
            raise ValueError('overlapping load segments')
        spans.append((physical, physical + size))
        if physical == FLASH_START and size >= 8:
            vector = struct.unpack_from('<II', data, offset)
        if flags & 1 and virtual <= entry - 1 < virtual + size:
            if physical != virtual:
                raise ValueError('reset code must be programmed at its execution address')
            executable_entry = True
    if not spans or vector is None or not executable_entry:
        raise ValueError('missing vector table or executable entry segment')
    stack, reset = vector
    if stack != 0x20010000 or reset != entry:
        raise ValueError('vector table does not match 64 KiB NK60 SRAM / ELF entry')
    return {'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data),
            'flash_segments': [[hex(a), hex(b)] for a, b in spans],
            'entry': hex(entry), 'stack': hex(stack)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--execute', action='store_true')
    args = parser.parse_args()
    image = args.image.resolve()
    try:
        info = validate_image(image)
    except (OSError, ValueError, struct.error) as error:
        parser.error(str(error))
    # OpenOCD parses Tcl; keep the validated, generated path out of Tcl syntax.
    # A conservative whitelist also makes the printed command directly usable.
    if any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_./-' for c in str(image)):
        parser.error('image path must contain only letters, digits, underscore, dot, slash or hyphen')
    command = ['openocd', '-f', str(ROOT / 'boards/nk60_v2/openocd.cfg'),
               '-c', 'program ' + str(image) + ' verify reset exit']
    print(json.dumps({'image': str(image), **info, 'execute': args.execute,
                      'command': command}, indent=2), flush=True)
    if args.execute:
        if not shutil.which('openocd'):
            parser.error('install OpenOCD and connect ST-Link before using --execute')
        return subprocess.run(command, cwd=ROOT).returncode
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
