#!/usr/bin/env python3
# Copyright (C) 2026 NOS contributors
"""Embed independently linked ELF files as a read-only kernel boot archive."""
import argparse
import json
import struct
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output", required=True, type=Path)
parser.add_argument("--file", action="append", default=[], metavar="PATH=ELF")
parser.add_argument("--negative-fixtures", action="store_true",
                    help="embed malformed copies of the first ELF for loader regression")
parser.add_argument("--path-max", type=int, choices=(64, 256), default=64,
                    help="archive path limit including NUL; Linux profiles use 256")
args = parser.parse_args()
lines = ['/* Generated initramfs; Copyright (C) 2026 NOS contributors */',
         '#include "elf.h"', '']
entries = []
images = [(item.split("=", 1)[0], Path(item.split("=", 1)[1]).read_bytes()) for item in args.file]
if args.negative_fixtures and images:
    original = images[0][1]
    is64 = original[4] == 2
    phoff = struct.unpack_from('<Q' if is64 else '<I', original, 32 if is64 else 28)[0]
    entsize, count = struct.unpack_from('<HH', original, 54 if is64 else 42)
    loads = [phoff + i * entsize for i in range(count)
             if struct.unpack_from('<I', original, phoff + i * entsize)[0] == 1]
    offsets = {'offset': 8 if is64 else 4, 'va': 16 if is64 else 8,
               'filesz': 32 if is64 else 16, 'memsz': 40 if is64 else 20,
               'align': 48 if is64 else 28}
    word_format = '<Q' if is64 else '<I'
    def field(data, header, name):
        return struct.unpack_from(word_format, data, header + offsets[name])[0]
    def set_field(data, header, name, value):
        struct.pack_into(word_format, data, header + offsets[name], value)
    writable = next(at for at in loads if struct.unpack_from('<I', original, at + (4 if is64 else 24))[0] & 2)
    pie = struct.unpack_from('<H', original, 16)[0] == 3
    cases = ['bounds', 'wx', 'entry', 'overlap', 'machine', 'tls', 'entry-bss',
             'segment-wrap', 'metadata-bounds', 'tls-escape', 'exec-stack']
    if pie:
        cases += ['rel-text', 'dynamic-unterminated', 'dynamic-bss', 'rel-table-bss']
        dynamic = next(phoff + i * entsize for i in range(count)
                       if struct.unpack_from('<I', original, phoff + i * entsize)[0] == 2)
        dynamic_offset = field(original, dynamic, 'offset')
        dynamic_bytes = field(original, dynamic, 'filesz')
        tags = {struct.unpack_from('<I', original, at)[0]: at
                for at in range(dynamic_offset, dynamic_offset + dynamic_bytes, 8)}
        rel_tag = 7 if struct.unpack_from('<H', original, 18)[0] == 243 else 17
        rel_va = struct.unpack_from('<I', original, tags[rel_tag] + 4)[0]
        rel_segment = next(at for at in loads if field(original, at, 'va') <= rel_va <
                           field(original, at, 'va') + field(original, at, 'filesz'))
        rel_file = field(original, rel_segment, 'offset') + rel_va - field(original, rel_segment, 'va')
    for case in cases:
        bad = bytearray(original)
        if case == 'bounds':
            struct.pack_into('<Q' if is64 else '<I', bad, 32 if is64 else 28, len(bad) + 1)
        elif case == 'wx':
            struct.pack_into('<I', bad, loads[0] + (4 if is64 else 24), 7)
        elif case == 'entry':
            struct.pack_into('<Q' if is64 else '<I', bad, 24, 0xdead0000)
        elif case == 'overlap' and len(loads) > 1:
            va = struct.unpack_from('<Q' if is64 else '<I', bad, loads[0] + (16 if is64 else 8))[0]
            struct.pack_into('<Q' if is64 else '<I', bad, loads[1] + (16 if is64 else 8), va)
        elif case == 'machine':
            struct.pack_into('<H', bad, 18, 0xffff)
        elif case == 'tls':
            # Reuse an existing non-load header for an oversized PT_TLS.
            at = phoff
            struct.pack_into('<I', bad, at, 7)
            struct.pack_into('<Q' if is64 else '<I', bad, at + (40 if is64 else 20), 4097)
        elif case == 'entry-bss':
            file_end = field(bad, loads[0], 'va') + field(bad, loads[0], 'filesz')
            entry = (file_end + 3) & ~3
            set_field(bad, loads[0], 'memsz', entry - field(bad, loads[0], 'va') + 16)
            if pie and struct.unpack_from('<H', bad, 18)[0] == 40:
                entry |= 1
            struct.pack_into(word_format, bad, 24, entry)
        elif case == 'segment-wrap':
            set_field(bad, loads[0], 'memsz', (1 << (64 if is64 else 32)) - 1)
        elif case == 'metadata-bounds':
            set_field(bad, phoff, 'offset', len(bad) + 1)
            set_field(bad, phoff, 'filesz', 4)
        elif case == 'tls-escape':
            struct.pack_into('<I', bad, phoff, 7)
            set_field(bad, phoff, 'offset', field(bad, writable, 'offset'))
            set_field(bad, phoff, 'va', field(bad, writable, 'va') + 4096)
            set_field(bad, phoff, 'filesz', 4)
            set_field(bad, phoff, 'memsz', 4)
            set_field(bad, phoff, 'align', 4)
        elif case == 'exec-stack':
            struct.pack_into('<I', bad, phoff, 0x6474e551)
            struct.pack_into('<I', bad, phoff + (4 if is64 else 24), 7)
        elif case == 'rel-text':
            struct.pack_into('<I', bad, rel_file, field(bad, loads[0], 'va') + 24)
        elif case == 'dynamic-unterminated':
            for at in range(dynamic_offset, dynamic_offset + dynamic_bytes, 8):
                if struct.unpack_from('<I', bad, at)[0] == 0:
                    struct.pack_into('<II', bad, at, 21, 0)
        elif case == 'dynamic-bss':
            start = (field(bad, writable, 'va') + field(bad, writable, 'filesz') + 3) & ~3
            set_field(bad, writable, 'memsz', start - field(bad, writable, 'va') + dynamic_bytes)
            set_field(bad, dynamic, 'va', start)
        elif case == 'rel-table-bss':
            start = (field(bad, writable, 'va') + field(bad, writable, 'filesz') + 3) & ~3
            set_field(bad, writable, 'memsz', start - field(bad, writable, 'va') + 16)
            struct.pack_into('<I', bad, tags[rel_tag] + 4, start)
        images.append((f'/test/elf-{case}', bytes(bad)))
for i, (name, data) in enumerate(images):
    if (not name.startswith("/") or '\0' in name or len(name.encode('utf-8')) >= args.path_max or
            any(len(component.encode('utf-8')) > 63 for component in name.split('/')) or
            name in [entry[0] for entry in entries]):
        parser.error(f"invalid or duplicate archive path: {name}")
    if not data.startswith(b"\x7fELF"):
        parser.error(f"not an ELF image: {name}")
    lines.append(f'static const unsigned char image_{i}[] __attribute__((aligned(16))) = {{')
    for offset in range(0, len(data), 24):
        lines.append('    ' + ','.join(f'0x{b:02x}' for b in data[offset:offset + 24]) + ',')
    lines.append('};')
    entries.append((name, i))
if args.negative_fixtures and entries:
    entries.append(('/abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ', 0))
    if args.path_max == 256:
        entries.append(('/' + '/'.join([letter * 50 for letter in 'abcd'] + ['hello']), 0))
if entries:
    lines += ['static int same(const char *a, const char *b)',
              '{ while (*a && *a == *b) { a++; b++; } return *a == *b; }']
lines += ['const void *nos_initramfs_find(const char *path, size_t *bytes)', '{',
          '    if (!path || !bytes) return 0;']
for name, i in entries:
    lines.append(f'    if (same(path, {json.dumps(name)})) {{ *bytes = sizeof(image_{i}); return image_{i}; }}')
lines += ['    *bytes = 0; return 0;', '}', '']
lines += ['const char *nos_initramfs_entry(size_t index, size_t *bytes)', '{', '    if (!bytes) return 0;', '    switch (index) {']
for number, (name, i) in enumerate(entries):
    lines.append(f'    case {number}: *bytes = sizeof(image_{i}); return {json.dumps(name)};')
lines += ['    default: *bytes = 0; return 0;', '    }', '}', '']
args.output.parent.mkdir(parents=True, exist_ok=True)
contents = '\n'.join(lines)
if not args.output.is_file() or args.output.read_text() != contents:
    args.output.write_text(contents)
