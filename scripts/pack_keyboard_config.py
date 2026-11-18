#!/usr/bin/env python3
# Copyright (C) 2026 Nick Hu <huqihan@live.com>
"""Pack a keyboard JSON document into NLK KEYBOARD 2.0 TLV for explicit import.

This is an interchange document, not a file to push into the managed filesystem.
Use the NLK configuration import command to validate, save and activate it.
"""
import argparse
import json
from pathlib import Path
import re
import struct


def string(value, limit=255):
    data = value.encode('utf-8')
    if len(data) > limit or b'\0' in data:
        raise ValueError(f'invalid string: {value!r}')
    return bytes([len(data)]) + data


def record(tag, value):
    return struct.pack('<HH', tag, len(value)) + value


def target(value):
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,23}', value):
        raise ValueError('target must be an object ID (1–23 ASCII letters, digits, _ or -)')
    return string(value)


def pack(config):
    body = bytearray(record(1, string(config['name'], 63) + string(config.get('description', ''), 127)))
    kind = config.get('kind', 'base')
    if kind == 'base':
        layers = [[int(code, 16) for row in layer for code in row.split()] for layer in config['layers']]
        if not 1 <= len(layers) <= 8 or not 1 <= len(layers[0]) <= 128 or any(len(l) != len(layers[0]) for l in layers):
            raise ValueError('each layer must cover the same physical keys')
        if any(not 0 <= code <= 255 for layer in layers for code in layer):
            raise ValueError('keycodes are HID values; put action parameters in arguments')
        chains = {}
        for action in config.get('actions', []):
            layer, key, name = action['layer'], action['key'], action['name']
            if not 0 <= layer < len(layers) or not 0 <= key < len(layers[0]):
                raise ValueError('action is outside the keymap')
            args = action.get('arguments', {})
            if name in ('config_select', 'macro_play'):
                encoded_args = target(args['target'])
            elif name == 'key_chord':
                encoded_args = struct.pack('<B', args['modifiers'])
            elif args:
                # Custom definitions may provide their own opaque argument bytes.
                encoded_args = bytes.fromhex(args['hex'])
            else:
                encoded_args = b''
            chain = chains.setdefault((layer, key), [])
            chain.append(string(name, 31) + struct.pack('<H', len(encoded_args)) + encoded_args)
            if len(chain) > 4:
                raise ValueError('at most four action bindings per key')
        debounce, rate = config['debounce_ms'], config['poll_rate_hz']
        if not 1 <= debounce <= 100 or not 1 <= rate <= 1000:
            raise ValueError('debounce must be 1–100 ms and poll rate 1–1000 Hz')
        if not 0 <= config.get('default_layer', 0) < len(layers):
            raise ValueError('default layer is outside the keymap')
        body += record(5, string(config['layout'], 31) + struct.pack('<HBBHBBBB',
            config.get('layout_version', 1), config.get('default_layer', 0), debounce, rate,
            int(config.get('nkro', True)), int(config.get('win_lock', False)),
            int(config.get('fn_lock', False)), config.get('sleep_timeout_min', 0)))
        for layer, codes in enumerate(layers):
            for key, code in enumerate(codes):
                chain = chains.get((layer, key), [])
                body += record(6, struct.pack('<BHIB', layer, key, code, len(chain)) + b''.join(chain))
    elif kind == 'macro':
        steps = config['steps']
        if len(steps) > 32:
            raise ValueError('at most 32 macro steps')
        for step in steps:
            op = {'down': 1, 'up': 2, 'tap': 3, 'delay': 4}[step['op']]
            code = step.get('keycode', 0)
            if op != 4 and not 1 <= code <= 0xe7:
                raise ValueError('macro key must be a USB HID usage')
            body += record(0x21, struct.pack('<BBH', op, code, step.get('delay_ms', 0)))
    else:
        raise ValueError('supported JSON producers: base and macro; lighting uses the definition-driven editor')
    if len(body) > 8192:
        raise ValueError('document exceeds device object limit')
    return body


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    try:
        data = pack(json.loads(args.source.read_text(encoding='utf-8')))
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(data)
    except (OSError, ValueError, KeyError, TypeError, struct.error) as exc:
        parser.error(str(exc))


if __name__ == '__main__':
    main()
