#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Sanitize the actual NK60 device whitelist and DMA range policy."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
INPUTS = ['tests/host_nk60_device.c', 'include/nos/nk60_device.h']
REQUIRED = {
    'nk60_mmio_valid_registers_and_widths',
    'nk60_mmio_dma_core_registers_and_aliases_denied',
    'nk60_dma_sram_edges_overflow_and_exact_length',
}


def main():
    out = ROOT / 'out/microkernel/host-nk60-device' / str(time.time_ns())
    out.mkdir(parents=True)
    hashes = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in INPUTS}
    command = ['gcc', '-Iinclude/nos', '-Wall', '-Wextra', '-Werror', '-std=gnu11',
               '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               'tests/host_nk60_device.c',
               '-o', str(out / 'host-nk60-device')]
    with (out / 'build.log').open('w') as stream:
        build = subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT)
    code = None
    if not build.returncode:
        environment = os.environ.copy()
        environment.update(ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
                           UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        with (out / 'host.log').open('w') as stream:
            try:
                run = subprocess.run([str(out / 'host-nk60-device')], cwd=ROOT, env=environment,
                                     stdout=stream, stderr=subprocess.STDOUT, timeout=60)
                code = run.returncode
            except subprocess.TimeoutExpired:
                pass
    text = (out / 'host.log').read_text() if (out / 'host.log').exists() else ''
    checks = {line[5:] for line in text.splitlines() if line.startswith('PASS ')}
    stable = hashes == {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in INPUTS}
    good = build.returncode == 0 and code == 0 and stable and REQUIRED <= checks and 'FAIL' not in text
    result = {'passed': good, 'scope': 'Actual NK60 MMIO whitelist and DMA address/length policy only; hardware DMA execution and syscall user-copy integration require board/QEMU validation',
              'build_command': command, 'build_returncode': build.returncode,
              'returncode': code, 'required': sorted(REQUIRED), 'missing': sorted(REQUIRED - checks),
              'source_stable': stable, 'source_sha256': hashes,
              'sanitizers': ['address', 'undefined']}
    (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))
    if not good:
        print((out / 'build.log').read_text()[-5000:] if build.returncode else text[-5000:])
    print(out / 'results.json')
    return 0 if good else 1


if __name__ == '__main__':
    raise SystemExit(main())
