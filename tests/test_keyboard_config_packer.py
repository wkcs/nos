"""Copyright (C) 2026 Nick Hu. NLK 键盘配置打包器与名称规则回归测试。"""
import importlib.util
import json
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('keyboard_packer', ROOT / 'scripts/pack_keyboard_config.py')
PACKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKER)


def records(body):
    result = []
    offset = 0
    while offset < len(body):
        tag, length = struct.unpack_from('<HH', body, offset)
        offset += 4
        value = body[offset:offset + length]
        if len(value) != length:
            raise ValueError('truncated record')
        result.append((tag, value))
        offset += length
    return result


class KeyboardConfigPackerTests(unittest.TestCase):
    def test_board_presets_are_complete_documents(self):
        for board in ('nk60-v1', 'nk60-v2'):
            config = json.loads((ROOT / f'configs/keyboard/{board}/macos.json').read_text())
            doc = records(PACKER.pack(config))
            self.assertEqual(sum(tag == 1 for tag, _ in doc), 1)
            self.assertEqual(sum(tag == 5 for tag, _ in doc), 1)
            self.assertEqual(sum(tag == 6 for tag, _ in doc), 61 * 3)
            header = next(value for tag, value in doc if tag == 5)
            self.assertTrue(header.startswith(b'\x06ansi61\x01\x00'))

    def test_full_targets_and_duplicate_action_arguments(self):
        config = dict(
            name='测试 配置',
            layout='ansi61',
            layers=[['04']],
            debounce_ms=5,
            poll_rate_hz=1000,
            actions=[
                dict(layer=0, key=0, name='config_select', arguments=dict(target='办公模式')),
                dict(layer=0, key=0, name='macro_play', arguments=dict(target='a' * 27)),
                dict(layer=0, key=0, name='key_chord', arguments=dict(modifiers=8)),
            ],
        )
        key = next(value for tag, value in records(PACKER.pack(config)) if tag == 6)
        self.assertEqual(key[7], 3)
        self.assertIn(bytes([len('办公模式'.encode('utf-8'))]) + '办公模式'.encode('utf-8'), key)
        self.assertIn(b'\x1b' + b'a' * 27, key)
        self.assertTrue(key.endswith(b'key_chord\x01\x00\x08'))
        config['default_layer'] = 1
        with self.assertRaises(ValueError):
            PACKER.pack(config)

    def test_name_validation_boundaries(self):
        for valid in ('a', 'Work Space', '一二三四五六七八九', 'a' * 27):
            self.assertEqual(PACKER.validate_name(valid), valid)
        for invalid in (
            '',
            'a' * 28,
            '一二三四五六七八九十',
            'bad/name',
            'bad\\name',
            'bad:name',
            'bad*name',
            'bad?name',
            'bad"name',
            'bad<name',
            'bad>name',
            'bad|name',
            '.hidden',
            'dot.',
            ' lead',
            'trail ',
            '~factory',
            'line\nbreak',
            'ctrl\x7f',
        ):
            with self.assertRaises(ValueError, msg=f'should reject {invalid!r}'):
                PACKER.validate_name(invalid)

    def test_macro_and_invalid_input(self):
        config = dict(kind='macro', name='Paste', steps=[dict(op='tap', keycode=4), dict(op='delay', delay_ms=50)])
        doc = records(PACKER.pack(config))
        self.assertEqual(doc[1:], [(0x21, b'\x03\x04\x00\x00'), (0x21, b'\x04\x00\x32\x00')])
        config['steps'] *= 17
        with self.assertRaises(ValueError):
            PACKER.pack(config)
        with self.assertRaises(ValueError):
            PACKER.target('hash/target')


if __name__ == '__main__':
    unittest.main()
