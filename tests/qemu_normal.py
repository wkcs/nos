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
    parser.add_argument('--timeout', type=int, default=60)
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
    # Assert output separately for each command: an echoed input must never satisfy
    # its own result, and ANSI redraws/history must never count as command prompts.
    commands = [
        ('quoted_write', b'write /normal "uart-proof"\n', r'^write: 10$'),
        ('read_file', b'cat /normal\n', r'^uart-proof$'),
        ('quote_escape_empty', b'echo "two words" \'single words\' escaped\\ value "" tail\n',
         r'^two words single words escaped value  tail$'),
        ('mkdir', b'mkdir /shell-dir\n', r'^mkdir: 0$'),
        ('cd', b'cd /shell-dir\n', None),
        ('pwd', b'pwd\n', r'^/shell-dir$'),
        ('relative_write', b'write local "relative data"\n', r'^write: 13$'),
        ('relative_read', b'cat ./local\n', r'^relative data$'),
        ('cd_parent', b'cd ..\n', None),
        ('pwd_parent', b'pwd\n', r'^/$'),
        ('relative_remove', b'rm /shell-dir/local\n', r'^rm: 0$'),
        ('rmdir', b'rmdir /shell-dir\n', r'^rmdir: 0$'),
        ('unmatched_quote', b'write /rejected "unterminated\n', r'^parse: unmatched quote$'),
        ('quote_no_side_effect', b'cat /rejected\n', r'^cat: -\d+$'),
        ('trailing_escape', b'echo unfinished\\\n', r'^parse: trailing escape$'),
        ('argument_limit', b'write /rejected a b c d e f g h\n', r'^parse: too many arguments$'),
        ('argument_no_side_effect', b'cat /rejected\n', r'^cat: -\d+$'),
        ('insert_at_cursor', b'echo edtor\033[D\033[D\033[Di\n', r'^editor$'),
        ('history_recall', b'\033[A\n', r'^editor$'),
        ('history_draft', b'echo draft\033[A\033[B\n', r'^draft$'),
        ('delete_at_cursor', b'echo deXlete\033[D\033[D\033[D\033[D\033[D\033[3~\n', r'^delete$'),
        ('backspace', b'echo okayX\x7f\n', r'^okay$'),
        ('clear_line', b'write /rejected nope\x15echo cleared\n', r'^cleared$'),
        ('cancel_line', b'write /rejected nope\x03', r'\^C$'),
        ('cancel_no_side_effect', b'cat /rejected\n', r'^cat: -\d+$'),
        ('crlf_once', b'echo crlf-proof\r\n', r'^crlf-proof$'),
        ('run_elf', b'run /bin/hello "shell argument"\n', r'^program: 23$'),
        ('stats_before_restart', b'stats\n', r'^uart_ready: 1$'),
        ('restart_first', b'restart\n', r'^restart: \d+$'),
        ('stats_after_first', b'stats\n', r'^uart_ready: 1$'),
        ('restart_second', b'restart\n', r'^restart: \d+$'),
        ('stats_after_second', b'stats\n', r'^uart_ready: 1$'),
        ('write_after_restart', b'write /after uart-proof\n', r'^write: 10$'),
        ('read_after_restart', b'cat /after\n', r'^uart-proof$'),
        ('stats_after_write', b'stats\n', r'^uart_ready: 1$'),
    ]
    sent = 0
    completed = []
    command_start = None
    command_failure = None
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
            text = output.decode(errors='replace').replace('\r\n', '\n')
            if not args.boot_only and command_start is not None:
                response = text[command_start:]
                # Successful submission and Ctrl-C both emit a newline. Redraws
                # before submission do not, even when they include 'nos> '.
                if '\n' in response and response.endswith('nos> '):
                    name, _, expected = commands[sent - 1]
                    if expected and not re.search(expected, response, re.MULTILINE):
                        command_failure = {'command': name, 'expected': expected, 'response': response}
                        break
                    if name == 'crlf_once' and response.count('nos> ') != 1:
                        command_failure = {'command': name, 'reason': 'CRLF submitted more than once', 'response': response}
                        break
                    completed.append(name)
                    command_start = None
            if not args.boot_only and command_start is None and sent < len(commands) and text.endswith('nos> '):
                command_start = len(text)
                process.stdin.write(commands[sent][1])
                process.stdin.flush()
                sent += 1
            finished = args.boot_only or len(completed) == len(commands)
            if finished and all(marker in text for marker in required) and uart_ok(text):
                # A normal init must remain runnable after successful application exit.
                time.sleep(0.25)
                break
        text = output.decode(errors='replace').replace('\r\n', '\n')
        stats = re.findall(r'tasks: (\d+)\nmutexes: (\d+)\nendpoints: (\d+)\nfree_pages: (\d+)', text)
        restarts = re.findall(r'^restart: (\d+)$', text, re.MULTILINE)
        resources_ok = args.boot_only or (len(stats) == 4 and len(set(stats)) == 1 and
                                         len(restarts) == 2 and restarts[0] != restarts[1])
        shell_ok = args.boot_only or (len(completed) == len(commands) and command_failure is None)
        good = process.poll() is None and all(marker in text for marker in required) and 'FAIL' not in text and uart_ok(text) and resources_ok and shell_ok
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait()
    (out / 'qemu.log').write_text(text)
    report = {'platform': args.platform, 'libc': args.libc, 'build_mode': 'normal',
              'passed': good, 'missing': [marker for marker in required if marker not in text],
              'checks': ['independent_elf_boot', 'persistent_init'] + ([] if args.boot_only else ['uart_shell_write_read', 'resource_stats', 'quoted_arguments', 'cwd_relative_paths', 'parser_rejection_no_side_effects', 'ansi_editor_history', 'crlf_single_submission', 'native_elf_execution']),
              'shell_commands_expected': len(commands) if not args.boot_only else 0,
              'shell_commands_completed': completed, 'shell_command_failure': command_failure,
              'uart_input_tested': not args.boot_only, 'uart_irq_metrics': uart_metrics(text),
              'service_restarts': restarts, 'resource_snapshots': stats, 'service_restart_resources_verified': resources_ok if not args.boot_only else None}
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))
    if not good:
        print(text[-7000:])
    return 0 if good else 1


if __name__ == '__main__':
    raise SystemExit(main())
