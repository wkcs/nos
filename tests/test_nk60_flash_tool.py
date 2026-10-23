#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Test SWD image validation, without hardware."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('flash_nk60_v2', ROOT / 'scripts/flash_nk60_v2.py')
FLASH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FLASH)
BASE = 0x08000000
ENTRY = BASE + 9


def segment(payload, physical=BASE, virtual=None, flags=5, memory=None, kind=1):
    return {'payload': payload, 'physical': physical,
            'virtual': physical if virtual is None else virtual, 'flags': flags,
            'memory': len(payload) if memory is None else memory, 'kind': kind}


def vectors(stack=0x20010000, reset=ENTRY):
    return struct.pack('<II', stack, reset) + b'\x00\xbf' * 4


def elf(segments=None, entry=ENTRY):
    if segments is None:
        segments = [segment(vectors())]
    result = bytearray(52 + 32 * len(segments))
    result[:16] = b'\x7fELF\x01\x01\x01' + bytes(9)
    struct.pack_into('<HHIIIIIHHHHHH', result, 16,
                     2, 40, 1, entry, 52, 0, 0x05000000,
                     52, 32, len(segments), 0, 0, 0)
    for index, item in enumerate(segments):
        offset = len(result)
        payload = item['payload']
        struct.pack_into('<IIIIIIII', result, 52 + 32 * index,
                         item['kind'], offset, item['virtual'], item['physical'],
                         len(payload), item['memory'], item['flags'], 4)
        result.extend(payload)
    return result


class FlashImageTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='nos-nk60-flash-')
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / 'fixture.elf'

    def validate(self, image):
        self.path.write_bytes(image)
        return FLASH.validate_image(self.path)

    def reject(self, image, message=None):
        with self.assertRaisesRegex(ValueError, message or '.'):
            self.validate(image)

    def test_valid_thumb_image(self):
        info = self.validate(elf())
        self.assertEqual(info['entry'], hex(ENTRY))
        self.assertEqual(info['stack'], '0x20010000')
        self.assertEqual(info['flash_segments'], [[hex(BASE), hex(BASE + 16)]])
        self.assertEqual(len(info['sha256']), 64)

    def test_ram_data_load_address_is_valid(self):
        info = self.validate(elf([segment(vectors()),
                                  segment(b'data', BASE + 256, 0x20000000, flags=6, memory=32)]))
        self.assertEqual(len(info['flash_segments']), 2)

    def test_bss_only_segment_does_not_write_flash(self):
        self.validate(elf([segment(vectors()),
                           segment(b'', 0x20000000, memory=32, flags=6)]))

    def test_rejects_truncated_header_and_wrong_identification(self):
        for image in (b'', elf()[:51], b'not an elf' + bytes(100)):
            with self.subTest(length=len(image)):
                self.reject(image)
        for offset, value in ((4, 2), (5, 2), (6, 0)):
            image = elf()
            image[offset] = value
            with self.subTest(offset=offset):
                self.reject(image)

    def test_rejects_wrong_architecture_kind_and_version(self):
        for offset, fmt, value in ((18, '<H', 183), (18, '<H', 243),
                                   (16, '<H', 3), (20, '<I', 0)):
            image = elf()
            struct.pack_into(fmt, image, offset, value)
            with self.subTest(offset=offset, value=value):
                self.reject(image)

    def test_rejects_invalid_program_header_table(self):
        for offset, fmt, value in ((28, '<I', 0), (28, '<I', 0xfffffffe),
                                   (42, '<H', 16), (44, '<H', 0),
                                   (44, '<H', 33), (44, '<H', 2)):
            image = elf()
            struct.pack_into(fmt, image, offset, value)
            with self.subTest(offset=offset, value=value):
                self.reject(image)

    def test_rejects_non_thumb_and_outside_entry(self):
        for entry in (ENTRY & ~1, BASE - 1, FLASH.FLASH_LIMIT + 1, 0x20000001):
            with self.subTest(entry=hex(entry)):
                self.reject(elf(entry=entry))

    def test_rejects_truncated_load_data(self):
        self.reject(elf()[:-1])
        image = elf()
        struct.pack_into('<I', image, 56, 0xfffffff0)  # p_offset
        self.reject(image)

    def test_rejects_file_size_larger_than_memory_size(self):
        self.reject(elf([segment(vectors(), memory=8)]))

    def test_rejects_loads_outside_reserved_application_partition(self):
        for physical in (BASE - 4, FLASH.FLASH_LIMIT, FLASH.FLASH_LIMIT - 3, 0x20000000):
            with self.subTest(physical=hex(physical)):
                self.reject(elf([segment(vectors()), segment(b'data', physical, flags=6)]))

    def test_rejects_overlapping_programmed_ranges(self):
        self.reject(elf([segment(vectors()), segment(b'collision', BASE + 12)]))
        self.reject(elf([segment(vectors()), segment(b'collision', BASE)]))

    def test_rejects_missing_or_short_vector_table(self):
        self.reject(elf([segment(vectors(), BASE + 128)]))
        self.reject(elf([segment(b'short'), segment(b'codecode', BASE + 8)]))

    def test_rejects_stack_mismatch(self):
        for stack in (0x2000ffff, 0x20018000, 0, BASE):
            with self.subTest(stack=hex(stack)):
                self.reject(elf([segment(vectors(stack=stack))]))

    def test_rejects_reset_vector_mismatch(self):
        for reset in (ENTRY & ~1, ENTRY + 2, BASE + 0x101):
            with self.subTest(reset=hex(reset)):
                self.reject(elf([segment(vectors(reset=reset))]))

    def test_rejects_entry_without_file_backed_executable_code(self):
        self.reject(elf([segment(vectors(), flags=6)]))
        self.reject(elf([segment(vectors()[:8], memory=64)]))

    def test_rejects_executable_entry_loaded_at_different_physical_address(self):
        # A RAM data VMA with a flash LMA is legitimate; Thumb reset code needs
        # its bytes at the actual reset PC, not just a matching virtual range.
        image = elf([segment(vectors()[:8], flags=4),
                     segment(b'\x00\xbf' * 4, BASE + 256, BASE + 8)])
        self.reject(image)

    def test_accepts_adjacent_load_segments(self):
        self.validate(elf([segment(vectors()[:8], flags=4),
                           segment(b'\x00\xbf' * 4, BASE + 8)]))


if __name__ == '__main__':
    unittest.main()
