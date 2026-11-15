#!/usr/bin/env python3
# Copyright (C) 2026 Nick Hu <huqihan@live.com>
"""Pack a JSON keymap into a NOS version 5 keyboard object for /data."""

import argparse
import json
from pathlib import Path
import struct
import zlib


HEADER = struct.Struct("<4sHBBB3xII24s32s64s")


def fixed_string(text, width):
    encoded = text.encode("utf-8")
    if len(encoded) >= width or b"\0" in encoded:
        raise ValueError(f"{text!r} does not fit in {width} bytes")
    return encoded


def pack(config):
    layers = [[int(code, 16) for row in layer for code in row.split()]
              for layer in config["layers"]]
    if not 1 <= len(layers) <= 8 or not 1 <= len(layers[0]) <= 128:
        raise ValueError("unsupported number of layers or keys")
    key_count = len(layers[0])
    if any(len(layer) != key_count for layer in layers):
        raise ValueError("all layers must cover the same physical keys")

    actions = {}
    for action in config.get("actions", []):
        layer, key = action["layer"], action["key"]
        if not 0 <= layer < len(layers) or not 0 <= key < key_count:
            raise ValueError(f"action outside the keymap: {action}")
        chain = actions.setdefault((layer, key), [])
        if not action["name"] or len(chain) == 4:
            raise ValueError(f"invalid action chain: {action}")
        chain.append(fixed_string(action["name"], 16))

    # nlk_kb_params, nlk_kb_analog_cfg, nlk_kb_light_override.
    debounce = config["debounce_ms"]
    rate = config["poll_rate_hz"]
    if not 1 <= debounce <= 255 or not 1 <= rate <= 65535:
        raise ValueError("debounce and poll rate must be positive and fit in the file")
    params = struct.pack("<8B2H", debounce, min(255, max(1, 1000 // rate)),
                         1, 0, 0, 0, config["layout_id"], 0, rate, 0)
    analog = struct.pack("<6B2x", 20, 0, 3, 3, 2, 2)
    body = bytearray(params + analog + bytes(32))
    for layer, codes in enumerate(layers):
        for key, code in enumerate(codes):
            chain = actions.get((layer, key), [])
            body.extend(struct.pack("<IB", code, len(chain)))
            for name in chain:
                body.extend(struct.pack("<16s", name))
    body.append(0)  # No embedded macro records.

    header = HEADER.pack(b"NKC1", 5, 1, key_count, len(layers),
                         HEADER.size + len(body), 0,
                         fixed_string(config["hardware"], 24),
                         fixed_string(config["name"], 32),
                         fixed_string(config["description"], 64))
    image = bytearray(header + body)
    struct.pack_into("<I", image, 16, zlib.crc32(image))
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        image = pack(json.loads(args.source.read_text(encoding="utf-8")))
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(image)
    except (OSError, ValueError, KeyError, TypeError, struct.error) as exc:
        parser.error(str(exc))


if __name__ == "__main__":
    main()
