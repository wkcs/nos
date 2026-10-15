#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Verify kernel-origin traps fail safely."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from qemu_platforms import PLATFORMS, qemu_command
from qemu_guest import run_guest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', required=True, choices=PLATFORMS)
    parser.add_argument('--qemu-bindir', default=os.environ.get('QEMU_BINDIR', ''))
    parser.add_argument('--timeout', type=int, default=20)
    args = parser.parse_args()
    out = ROOT / 'out/microkernel/kernel-fault' / str(time.time_ns()) / args.platform
    out.mkdir(parents=True)
    with (out / 'build.log').open('w') as stream:
        build = subprocess.run(['make', '-j5', f'PLATFORM={args.platform}', 'LIBC=minimal',
                                'BUILD_MODE=selftest', 'EXTRA_CFLAGS=-DNOS_KERNEL_FAULT_INJECT',
                                f'O={out}'], cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT)
    code, text, metrics = None, '', {}
    if build.returncode == 0:
        try:
            text, code, metrics = run_guest(qemu_command(args.platform, out / 'nos.elf', args.qemu_bindir),
                                            out / 'qemu.log', args.timeout)
        except OSError as exc:
            text = str(exc)
    lines = text.splitlines()
    failures = [line for line in lines if line.startswith('FAIL')]
    expected = 'FAIL kernel: privileged_exception'
    good = build.returncode == 0 and code == 1 and 'PANIC privileged_exception' in lines and failures == [expected]
    report = {'platform': args.platform, 'passed': good, 'exit_code': code,
              'build_returncode': build.returncode, 'metrics': metrics,
              'required': ['PANIC privileged_exception', expected],
              'failures': failures, 'timeout_is_failure': True}
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))
    if not good:
        print((out / 'build.log').read_text()[-5000:] if build.returncode else text[-5000:])
    print(out / 'results.json')
    return 0 if good else 1


if __name__ == '__main__':
    raise SystemExit(main())
