#!/usr/bin/env python3
"""Generate immutable NOS device-tree data from a preprocessed DTS file."""
import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent / 'vendor' / 'zephyr_devicetree'))
import dtlib  # noqa: E402


def c_string(value):
    return json.dumps(value, ensure_ascii=True)


def generate(source, include_dirs=()):
    tree = dtlib.DT(str(source), include_path=include_dirs)
    nodes = list(tree.node_iter())
    indices = {node.path: index for index, node in enumerate(nodes)}
    lines = ['/* Generated from DTS; do not edit. */', '#include <nos/devicetree.h>', '']
    for index, node in enumerate(nodes):
        for prop_index, prop in enumerate(node.props.values()):
            if prop.value:
                data = ', '.join(f'0x{byte:02x}' for byte in prop.value)
                lines.append(f'static const uint8_t dt_value_{index}_{prop_index}[] = {{ {data} }};')
        if node.props:
            lines.append(f'static const struct nos_dt_property dt_props_{index}[] = {{')
            for prop_index, prop in enumerate(node.props.values()):
                data = f'dt_value_{index}_{prop_index}' if prop.value else 'NULL'
                lines.append(f'    {{ {c_string(prop.name)}, {data}, {len(prop.value)} }},')
            lines.append('};')
    lines.append('const struct nos_dt_node nos_dt_nodes[] = {')
    for index, node in enumerate(nodes):
        props = f'dt_props_{index}' if node.props else 'NULL'
        parent = indices[node.parent.path] if node.parent is not None else -1
        lines.append(f'    {{ {c_string(node.path)}, {props}, {len(node.props)}, {parent} }},')
    lines.extend(['};', f'const size_t nos_dt_node_count = {len(nodes)};', ''])
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--include-dir', action='append', default=[])
    args = parser.parse_args()
    try:
        result = generate(args.input, args.include_dir)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(result, encoding='utf-8')
    except (dtlib.DTError, OSError) as exc:
        print(f'device tree: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
