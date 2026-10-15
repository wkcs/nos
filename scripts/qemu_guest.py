# Copyright (C) 2026 NOS contributors
"""Bounded QEMU output collection with independent host clock measurements."""
import os
from pathlib import Path
import select
import subprocess
import time


def run_guest(command, log_path, timeout):
    log_path = Path(log_path)
    process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
    deadline = time.monotonic() + timeout
    clock, partial, total, reason = {}, bytearray(), 0, None
    try:
        with log_path.open('wb') as stream:
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    reason = 'timeout'; break
                ready, _, _ = select.select([process.stdout], [], [], min(0.1, remaining))
                if not ready:
                    continue
                chunk = os.read(process.stdout.fileno(), 65536)
                if not chunk:
                    break
                stream.write(chunk)
                total += len(chunk)
                if total > 4 * 1024 * 1024:
                    reason = 'output_limit'; break
                partial.extend(chunk)
                while b'\n' in partial:
                    line, _, tail = partial.partition(b'\n')
                    partial = bytearray(tail)
                    label = line.rstrip(b'\r').decode(errors='replace')
                    if label in ('NOS CLOCK START', 'NOS CLOCK END') and label not in clock:
                        clock[label] = time.monotonic()
                if len(partial) > 65536:
                    partial = partial[-65536:]
        if reason:
            process.kill()
        try:
            code = process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait(); code = None
            reason = reason or 'exit_timeout'
    finally:
        if process.poll() is None:
            process.kill(); process.wait()
        process.stdout.close()
    text = log_path.read_bytes()[-4 * 1024 * 1024:].decode(errors='replace')
    elapsed = None
    if 'NOS CLOCK START' in clock and 'NOS CLOCK END' in clock:
        elapsed = clock['NOS CLOCK END'] - clock['NOS CLOCK START']
    return text, None if reason else code, {'host_clock_seconds': elapsed, 'termination': reason,
                                           'output_bytes': total}
