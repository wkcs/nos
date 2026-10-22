#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Verify pending UART IRQ rearming in stock QEMU."""
import argparse
import json
import os
from pathlib import Path
import re
import select
import socket
import subprocess
import sys
import time

from qemu_matrix import ROOT, file_digest, git_identity, tool_version
from qemu_platforms import qemu_command

PLATFORMS = ('riscv_virt', 'riscv32_virt', 'riscv32_nommu')
FIRST = b'echo irq-first\n'
TRIGGER = b'\x1b'
PENDING = b'\x03echo irq-pending\n'


def debugger_script(image, port, input_fd, out):
    """Pause at the actual kernel unmask, inject UART data, and resume its code.

    The test reads PLIC state but never writes its registers. In particular it
    does not refresh the threshold itself: recovery must come from NOS.
    """
    python = f'''
import gdb, json, os, time
with open({str(out / 'input-events.json')!r}) as stream:
    events = json.load(stream)
def record(name, **values):
    events.append(dict(event=name, time_ns=time.time_ns(), **values))
    with open({str(out / 'input-events.json')!r}, 'w') as stream:
        json.dump(events, stream, indent=2)
def word(address):
    return int(gdb.parse_and_eval('*(unsigned*)%d' % address))
def byte(address):
    return int(gdb.parse_and_eval('*(unsigned char*)%d' % address))
breakpoint = gdb.Breakpoint('arch_irq_mask')
breakpoint.condition = 'irq == 10 && masked == 0'
written = os.write({input_fd}, {TRIGGER!r})
record('input_silent_escape', bytes_hex={TRIGGER.hex()!r}, written=written)
assert written == {len(TRIGGER)}, 'incomplete silent trigger write'
gdb.execute('continue')
assert int(gdb.parse_and_eval('irq')) == 10
assert int(gdb.parse_and_eval('masked')) == 0
# These are CPU0 supervisor-context PLIC registers on the stock virt boards.
enable_address, pending_address = 0x0c002080, 0x0c001000
enable_before = word(enable_address)
assert not enable_before & (1 << 10), 'breakpoint did not catch a masked UART'
written = os.write({input_fd}, {PENDING!r})
record('input_pending_while_masked', bytes_hex={PENDING.hex()!r}, written=written)
assert written == {len(PENDING)}, 'incomplete pending input write'
deadline = time.monotonic() + 3
while not word(pending_address) & (1 << 10):
    assert time.monotonic() < deadline, 'UART did not become pending while CPU paused'
    time.sleep(.01)
snapshot = dict(enable=word(enable_address), pending=word(pending_address),
                threshold=word(0x0c201000), uart_ier=byte(0x10000001),
                uart_lsr=byte(0x10000005), pc=int(gdb.parse_and_eval('$pc')),
                sip=int(gdb.parse_and_eval('$sip')))
assert not snapshot['enable'] & (1 << 10)
assert snapshot['pending'] & (1 << 10)
assert snapshot['uart_lsr'] & 1
assert snapshot['uart_ier'] & 1
snapshot['masked_pending_window_verified'] = True
with open({str(out / 'masked-pending.json')!r}, 'w') as stream:
    json.dump(snapshot, stream, indent=2)
print('UART_MASKED_PENDING_WINDOW_VERIFIED', snapshot)
breakpoint.delete()
record('resume_actual_nos_unmask_without_more_input')
gdb.execute('detach')
'''
    return '\n'.join(['set pagination off', 'set confirm off', 'set remotetimeout 5',
                      f'file {image}', f'target remote 127.0.0.1:{port}',
                      'python', python, 'end', 'quit']) + '\n'


def read_available(process, output, timeout):
    readable, _, _ = select.select([process.stdout], [], [], timeout)
    if readable:
        chunk = os.read(process.stdout.fileno(), 4096)
        if chunk:
            output.extend(chunk)
            return True
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=PLATFORMS, required=True)
    parser.add_argument('--qemu-bindir', default=os.environ.get('QEMU_BINDIR', ''))
    parser.add_argument('--gdb', default='gdb-multiarch')
    parser.add_argument('--timeout', type=float, default=10)
    parser.add_argument('--output-root', type=Path, default=ROOT / 'out/microkernel/uart-rearm')
    parser.add_argument('--image', type=Path, help='Use an existing image; its build-source identity is not inferred')
    args = parser.parse_args()
    out = args.output_root.resolve() / str(time.time_ns()) / args.platform
    out.mkdir(parents=True)
    source = git_identity()
    build_code = None
    image = args.image.resolve() if args.image else out / 'nos.elf'
    if not args.image:
        with (out / 'build.log').open('w') as log:
            build = subprocess.run(['make', '-j5', f'PLATFORM={args.platform}', 'LIBC=minimal',
                                    'BUILD_MODE=normal', f'O={out}'], cwd=ROOT,
                                   stdout=log, stderr=subprocess.STDOUT, timeout=180)
        build_code = build.returncode
    image_before = file_digest(image) if image.is_file() else None
    output = bytearray()
    process = None
    gdb_code = None
    failure = None
    alive = False
    cleanup_code = None
    command = None
    if args.image or build_code == 0:
        try:
            with socket.socket() as listener:
                listener.bind(('127.0.0.1', 0))
                port = listener.getsockname()[1]
            command = qemu_command(args.platform, image, args.qemu_bindir,
                                   extra_args=('-gdb', f'tcp:127.0.0.1:{port}'))
            process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT)
            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline and process.poll() is None:
                read_available(process, output, .1)
                if b'NOS SHELL READY\nnos> ' in output:
                    break
            else:
                raise RuntimeError('normal boot did not reach nshell before deadline')
            # Drain an entire ordinary command before arming the breakpoint.
            # Otherwise its later UART TX could refresh the PLIC and rescue an
            # unfixed image. A lone ESC triggers reception without any echo.
            written = process.stdin.write(FIRST)
            process.stdin.flush()
            if written != len(FIRST):
                raise RuntimeError('incomplete baseline input write')
            (out / 'input-events.json').write_text(json.dumps([
                {'event': 'input_first', 'bytes_hex': FIRST.hex(),
                 'written': written, 'time_ns': time.time_ns()}], indent=2))
            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline and process.poll() is None:
                read_available(process, output, .1)
                text = output.decode(errors='replace').replace('\r\n', '\n')
                if re.search(r'^irq-first$', text, re.MULTILINE) and text.endswith('nos> '):
                    break
            else:
                raise RuntimeError('baseline command did not complete before deadline')
            (out / 'debugger.gdb').write_text(debugger_script(image, port, process.stdin.fileno(), out))
            with (out / 'gdb.log').open('w') as log:
                debugger = subprocess.run([args.gdb, '-q', '-batch', '-x', str(out / 'debugger.gdb')],
                                          cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                                          pass_fds=(process.stdin.fileno(),), timeout=args.timeout + 5)
            gdb_code = debugger.returncode
            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline and process.poll() is None:
                read_available(process, output, .1)
                text = output.decode(errors='replace').replace('\r\n', '\n')
                if re.search(r'^irq-first$', text, re.MULTILINE) and re.search(r'^irq-pending$', text, re.MULTILINE) and text.endswith('nos> '):
                    break
            alive = process.poll() is None
        except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
            failure = str(exc)
        finally:
            if process is not None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                cleanup_code = process.returncode
    text = output.decode(errors='replace').replace('\r\n', '\n')
    (out / 'qemu.log').write_bytes(output)
    snapshot = json.loads((out / 'masked-pending.json').read_text()) if (out / 'masked-pending.json').is_file() else None
    events = json.loads((out / 'input-events.json').read_text()) if (out / 'input-events.json').is_file() else []
    checks = {'masked_pending_window': bool(snapshot and snapshot.get('masked_pending_window_verified')),
              'first_command_once': len(re.findall(r'^irq-first$', text, re.MULTILINE)) == 1,
              'pending_command_once': len(re.findall(r'^irq-pending$', text, re.MULTILINE)) == 1,
              'returned_to_prompt': text.endswith('nos> '), 'persistent_init': alive,
              'input_sequence_verified': [event['event'] for event in events] ==
                  ['input_first', 'input_silent_escape', 'input_pending_while_masked',
                   'resume_actual_nos_unmask_without_more_input']}
    source_after = git_identity()
    image_after = file_digest(image) if image.is_file() else None
    passed = (failure is None and (args.image is not None or build_code == 0) and gdb_code == 0
              and all(checks.values()) and 'FAIL' not in text and source == source_after
              and image_before is not None and image_before == image_after)
    report = {'passed': passed, 'platform': args.platform, 'checks': checks, 'failure': failure,
              'build_returncode': build_code, 'gdb_returncode': gdb_code,
              'guest_alive_before_controlled_termination': alive, 'guest_cleanup_returncode': cleanup_code,
              'image': str(image), 'image_sha256': image_before, 'image_sha256_after': image_after,
              'image_origin': 'provided_image_build_source_unknown' if args.image else 'built_from_reported_source',
              'source': source, 'source_after': source_after, 'source_changed_during_run': source != source_after,
              'qemu_command': command, 'qemu_version': tool_version(command[0]) if command else None,
              'gdb_version': tool_version(args.gdb), 'masked_pending_snapshot': snapshot,
              'scope': 'Stock QEMU UART pending while the real NOS IRQ ACK is masked; baseline command completes first, a silent ESC triggers ACK, debugger injects Ctrl-C plus the second command while masked, then resumes actual NOS unmask code without controller writes or further serial input'}
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: report[key] for key in ('passed', 'platform', 'checks', 'failure',
                                                  'image_sha256', 'image_origin', 'gdb_returncode')}))
    if not passed:
        print(text[-5000:])
        if (out / 'gdb.log').is_file():
            print((out / 'gdb.log').read_text()[-5000:])
    print(out / 'results.json')
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
