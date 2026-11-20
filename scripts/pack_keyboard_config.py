#!/usr/bin/env python3
# Copyright (C) 2026 Nick Hu <huqihan@live.com>
"""将键盘 JSON 配置打包为 NLK KEYBOARD 2.0 TLV 二进制文档。

此输出为跨端交换格式，请通过 NLK 配置导入命令校验、保存并启用，勿直接写入受管存储目录。
"""
import argparse
import json
from pathlib import Path
import struct

MAX_NAME_BYTES = 27
FORBIDDEN_CHARS = frozenset('/\\:*?"<>|')


def validate_name(value):
    if not isinstance(value, str) or not value:
        raise ValueError('配置名称不能为空')
    if value[0] in '. ~' or value[-1] in '. ':
        raise ValueError('配置名称不能以 .、空格开头或结尾，也不能以 ~ 开头')
    for ch in value:
        code = ord(ch)
        if code < 0x20 or code == 0x7f or ch in FORBIDDEN_CHARS:
            raise ValueError(f'配置名称包含非法字符: {value!r}')
    try:
        raw = value.encode('utf-8')
    except UnicodeEncodeError as exc:
        raise ValueError(f'配置名称不是合法 UTF-8: {value!r}') from exc
    if not 1 <= len(raw) <= MAX_NAME_BYTES:
        raise ValueError(f'配置名称 UTF-8 长度必须在 1–{MAX_NAME_BYTES} 字节之间')
    return value


def string(value, limit=255):
    data = value.encode('utf-8')
    if len(data) > limit or b'\0' in data:
        raise ValueError(f'非法字符串: {value!r}')
    return bytes([len(data)]) + data


def record(tag, value):
    return struct.pack('<HH', tag, len(value)) + value


def target(value):
    return string(validate_name(value), MAX_NAME_BYTES)


def pack(config):
    name = validate_name(config['name'])
    body = bytearray(record(1, string(name, MAX_NAME_BYTES) + string(config.get('description', ''), 127)))
    kind = config.get('kind', 'base')
    if kind == 'base':
        layers = [[int(code, 16) for row in layer for code in row.split()] for layer in config['layers']]
        if not 1 <= len(layers) <= 8 or not 1 <= len(layers[0]) <= 128 or any(len(l) != len(layers[0]) for l in layers):
            raise ValueError('各层必须覆盖相同数量的物理按键')
        if any(not 0 <= code <= 255 for layer in layers for code in layer):
            raise ValueError('键码须为单字节 HID 值；action 参数请写入 arguments')
        chains = {}
        for action in config.get('actions', []):
            layer, key, action_name = action['layer'], action['key'], action['name']
            if not 0 <= layer < len(layers) or not 0 <= key < len(layers[0]):
                raise ValueError('action 坐标超出按键矩阵范围')
            args = action.get('arguments', {})
            if action_name in ('config_select', 'macro_play'):
                encoded_args = target(args['target'])
            elif action_name == 'key_chord':
                encoded_args = struct.pack('<B', args['modifiers'])
            elif args:
                # 扩展动作可自带原始字节参数
                encoded_args = bytes.fromhex(args['hex'])
            else:
                encoded_args = b''
            chain = chains.setdefault((layer, key), [])
            chain.append(string(action_name, 31) + struct.pack('<H', len(encoded_args)) + encoded_args)
            if len(chain) > 4:
                raise ValueError('单个按键最多绑定四个 action')
        debounce, rate = config['debounce_ms'], config['poll_rate_hz']
        if not 1 <= debounce <= 100 or not 1 <= rate <= 1000:
            raise ValueError('消抖时间须为 1–100 ms，扫描率须为 1–1000 Hz')
        if not 0 <= config.get('default_layer', 0) < len(layers):
            raise ValueError('默认层索引越界')
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
            raise ValueError('宏最多支持 32 个步骤')
        for step in steps:
            op = {'down': 1, 'up': 2, 'tap': 3, 'delay': 4}[step['op']]
            code = step.get('keycode', 0)
            if op != 4 and not 1 <= code <= 0xe7:
                raise ValueError('宏按键须为有效 USB HID 键码')
            body += record(0x21, struct.pack('<BBH', op, code, step.get('delay_ms', 0)))
    else:
        raise ValueError('JSON 打包器仅支持 base 与 macro；灯光配置请使用定义驱动编辑器')
    if len(body) > 8192:
        raise ValueError('文档体积超出设备 8192 字节上限')
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
