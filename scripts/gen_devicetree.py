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


def load_bindings(directory):
    bindings = {}
    if directory is None:
        return bindings
    for path in sorted(directory.rglob('*.json')):
        binding = json.loads(path.read_text(encoding='utf-8'))
        compatible = binding['compatible']
        if compatible in bindings:
            raise ValueError(f'duplicate binding for {compatible}')
        bindings[compatible] = binding
    return bindings


def validate_bindings(nodes, bindings):
    for node in nodes:
        compatible = node.props.get('compatible')
        if compatible is None:
            continue
        binding = next((bindings[name] for name in compatible.to_strings() if name in bindings), None)
        if binding is None:
            continue
        for name, rule in binding.get('properties', {}).items():
            prop = node.props.get(name)
            if prop is None:
                if rule.get('required', False):
                    raise ValueError(f'{node.path}: missing required property {name}')
                continue
            kind = rule['type']
            if kind == 'uint32-array':
                value = prop.to_nums()
                if len(value) < rule.get('min-items', 0):
                    raise ValueError(f'{node.path}: property {name} has too few cells')
                if 'max-items' in rule and len(value) > rule['max-items']:
                    raise ValueError(f'{node.path}: property {name} has too many cells')
                if 'minimum' in rule and any(item < rule['minimum'] for item in value):
                    raise ValueError(f'{node.path}: property {name} is below the minimum')
            elif kind == 'string':
                value = prop.to_string()
            elif kind == 'boolean':
                if prop.type != dtlib.Type.EMPTY:
                    raise ValueError(f'{node.path}: property {name} must be boolean')
                value = True
            elif kind == 'phandle-array':
                if prop.type not in (dtlib.Type.PHANDLE, dtlib.Type.PHANDLES,
                                     dtlib.Type.PHANDLES_AND_NUMS):
                    raise ValueError(f'{node.path}: property {name} must contain phandles')
                value = prop.value
            else:
                raise ValueError(f'{node.path}: unsupported binding type {kind}')
            if 'enum' in rule and value not in rule['enum']:
                raise ValueError(f'{node.path}: invalid value for {name}: {value}')


def generate(source, include_dirs=(), bindings=None):
    tree = dtlib.DT(str(source), include_path=include_dirs)
    nodes = list(tree.node_iter())
    validate_bindings(nodes, bindings or {})
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
    parser.add_argument('--bindings', type=Path)
    args = parser.parse_args()
    try:
        result = generate(args.input, args.include_dir, load_bindings(args.bindings))
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(result, encoding='utf-8')
    except (dtlib.DTError, OSError, ValueError, KeyError) as exc:
        print(f'device tree: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
