#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Verify actual incremental guest builds."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--platform', default='arm64_virt')
    parser.add_argument('--libc', choices=('minimal', 'musl'), default='musl')
    args = parser.parse_args()
    # Each run owns its object tree; removed sources from earlier revisions must
    # not participate in this configuration's rebuild assertions.
    out = ROOT / 'out/microkernel/build-configuration' / args.platform / args.libc / str(time.time_ns())
    command = ['make', '-j5', f'PLATFORM={args.platform}', f'LIBC={args.libc}', f'O={out}',
               'BUILD_MODE=selftest']
    private_archive = None
    if args.libc == 'musl':
        original = ROOT / 'out/microkernel/musl' / args.platform / 'install'
        private = out / 'private-musl'
        private.mkdir(parents=True, exist_ok=True)
        include = private / 'include'
        if not include.exists():
            include.symlink_to(original / 'include', target_is_directory=True)
        (private / 'lib').mkdir(exist_ok=True)
        private_archive = private / 'lib/libc.a'
        shutil.copy2(original / 'lib/libc.a', private_archive)
        command.append(f'MUSL_ROOT={private}')
    def build(extra):
        subprocess.run(command + [f'EXTRA_CFLAGS={extra}'], cwd=ROOT, check=True,
                       stdout=subprocess.DEVNULL)
        return {str(path): path.stat().st_mtime_ns for path in out.rglob('*.o')}, (out / 'nos.elf').stat().st_mtime_ns
    first_objects, first_image = build('-DNOS_BUILD_PROBE=1')
    same_objects, same_image = build('-DNOS_BUILD_PROBE=1')
    assert first_objects == same_objects and first_image == same_image, 'Unchanged configuration rebuilt artifacts'
    second_objects, _ = build('-DNOS_BUILD_PROBE=2')
    assert all(second_objects[name] > stamp for name, stamp in first_objects.items()), 'Changed flags left stale objects'
    if private_archive:
        before_objects, before_image = build('-DNOS_BUILD_PROBE=2')
        os.utime(private_archive, ns=(time.time_ns(), time.time_ns()))
        after_objects, after_image = build('-DNOS_BUILD_PROBE=2')
        assert before_objects == after_objects, 'Archive update unnecessarily recompiled user/kernel sources'
        assert after_image > before_image, 'Changed static libc archive did not relink the image'
    result = {'passed': True, 'platform': args.platform, 'libc': args.libc,
              'checks': ['unchanged_no_rebuild', 'flags_recompile'] + (['static_archive_relink'] if private_archive else [])}
    (out / 'incremental-results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
