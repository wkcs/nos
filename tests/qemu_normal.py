#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Verify persistent init and interactive UART."""
import argparse
import json
import os
import re
from pathlib import Path
import select
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from qemu_platforms import PLATFORMS, qemu_command


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--platform', choices=PLATFORMS, default='arm64_virt')
    parser.add_argument('--libc', choices=('minimal', 'newlib', 'musl'), default='minimal')
    parser.add_argument('--qemu-bindir', default=os.environ.get('QEMU_BINDIR', ''))
    parser.add_argument('--timeout', type=int, default=20)
    parser.add_argument('--boot-only', action='store_true', help='Validate persistent init and ELF boot without exercising UART input')
    args = parser.parse_args()
    out = ROOT / 'out/microkernel/normal-validation' / args.platform / args.libc
    out.mkdir(parents=True, exist_ok=True)
    with (out / 'build.log').open('w') as log:
        build = subprocess.run(['make', '-j5', f'PLATFORM={args.platform}', f'LIBC={args.libc}',
                                f'O={out}', 'BUILD_MODE=normal'], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    if build.returncode:
        print((out / 'build.log').read_text()[-7000:])
        return 1
    process = subprocess.Popen(qemu_command(args.platform, out / 'nos.elf', args.qemu_bindir),
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    output = bytearray()
    commands = [b'write /normal uart-proof\n', b'cat /normal\n', b'stats\n', b'restart\n',
                b'stats\n', b'restart\n', b'stats\n', b'write /after uart-proof\n', b'cat /after\n', b'stats\n']
    sent = 0
    deadline = time.monotonic() + args.timeout
    required = ['NOS NORMAL BOOT', 'NOS ELF APP PASS', 'program: 23', 'NOS SHELL READY',
                'write: 10', 'uart-proof', 'tasks: ']
    if args.boot_only:
        required = required[:4]
    else:
        required += ['uart_irq: ', 'uart_rx: ', 'uart_dropped: ', 'uart_ready: ']
    def uart_metrics(text):
        return {name: int(matches[-1]) for name in ('irq', 'rx', 'dropped', 'ready')
                if (matches := re.findall(r'^uart_' + name + r': (\d+)$', text, re.MULTILINE))}
    def uart_ok(text):
        values = uart_metrics(text)
        return args.boot_only or (values.get('ready') == 1 and values.get('irq', 0) > 0 and
                                  values.get('rx', 0) > 0 and values.get('dropped') == 0)
    try:
        while time.monotonic() < deadline and len(output) < 4 * 1024 * 1024:
            readable, _, _ = select.select([process.stdout], [], [], 0.2)
            if readable:
                chunk = os.read(process.stdout.fileno(), 4096)
                if not chunk:
                    break
                output.extend(chunk)
            text = output.decode(errors='replace')
            if not args.boot_only and sent < len(commands) and text.count('nos> ') > sent:
                process.stdin.write(commands[sent])
                process.stdin.flush()
                sent += 1
            finished = args.boot_only or (sent == len(commands) and text.count('nos> ') > sent)
            if finished and all(marker in text for marker in required) and uart_ok(text):
                # A normal init must remain runnable after successful application exit.
                time.sleep(0.25)
                break
        text = output.decode(errors='replace')
        stats = re.findall(r'tasks: (\d+)\nmutexes: (\d+)\nendpoints: (\d+)\nfree_pages: (\d+)', text)
        restarts = re.findall(r'^restart: (\d+)$', text, re.MULTILINE)
        resources_ok = args.boot_only or (len(stats) == 4 and len(set(stats)) == 1 and
                                         len(restarts) == 2 and restarts[0] != restarts[1])
        good = process.poll() is None and all(marker in text for marker in required) and 'FAIL' not in text and uart_ok(text) and resources_ok
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait()
    (out / 'qemu.log').write_text(text)
    report = {'platform': args.platform, 'libc': args.libc, 'build_mode': 'normal',
              'passed': good, 'missing': [marker for marker in required if marker not in text],
              'checks': ['independent_elf_boot', 'persistent_init'] + ([] if args.boot_only else ['uart_shell_write_read', 'resource_stats']),
              'uart_input_tested': not args.boot_only, 'uart_irq_metrics': uart_metrics(text),
              'service_restarts': restarts, 'resource_snapshots': stats, 'service_restart_resources_verified': resources_ok if not args.boot_only else None}
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))
    if not good:
        print(text[-7000:])
    return 0 if good else 1


if __name__ == '__main__':
    raise SystemExit(main())
