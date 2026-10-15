#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Content based build configuration stamp."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def atomic_write_changed(path, text):
    path = Path(path)
    if path.exists() and path.read_text() == text:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode='w', dir=path.parent, delete=False) as stream:
        stream.write(text)
        temporary = stream.name
    os.replace(temporary, path)
    return True


def compiler_identity(compiler):
    resolved = shutil.which(compiler)
    if not resolved:
        raise SystemExit(f'Compiler unavailable: {compiler}')
    resolved = str(Path(resolved).resolve())
    version = subprocess.check_output([compiler, '--version'], text=True).splitlines()[0]
    machine = subprocess.check_output([compiler, '-dumpmachine'], text=True).strip()
    return {'command': compiler, 'path': resolved, 'sha256': digest(resolved),
            'version': version, 'machine': machine}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--value', action='append', default=[])
    parser.add_argument('--rule', action='append', default=[])
    args = parser.parse_args()
    state = {'schema_version': 1, 'compiler': compiler_identity(args.compiler),
             'configuration': dict(item.split('=', 1) for item in args.value),
             'rules': {name: digest(name) for name in args.rule if Path(name).is_file()}}
    atomic_write_changed(args.output, json.dumps(state, indent=2, sort_keys=True) + '\n')


if __name__ == '__main__':
    main()
