#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Sanitized VFS bounds tests with stubbed transport."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    out = ROOT / 'out/microkernel/host-vfs' / str(time.time_ns())
    out.mkdir(parents=True)
    binary = out / 'host-vfs'
    command = ['gcc', '-DNOS_MMU', '-Iinclude/nos', '-fsanitize=address,undefined',
               '-fno-omit-frame-pointer', '-ffunction-sections', '-fdata-sections',
               '-Wl,--gc-sections', '-g', '-O1', 'tests/host_vfs.c', '-o', str(binary)]
    with (out / 'build.log').open('w') as log:
        build = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    code = None
    if not build.returncode:
        environment = os.environ.copy()
        environment.update(ASAN_OPTIONS='detect_leaks=1:abort_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        with (out / 'host.log').open('w') as log:
            try:
                run = subprocess.run([str(binary)], cwd=ROOT, env=environment, stdout=log,
                                     stderr=subprocess.STDOUT, timeout=120)
                code = run.returncode
            except subprocess.TimeoutExpired:
                pass
    text = (out / 'host.log').read_text() if (out / 'host.log').exists() else ''
    good = build.returncode == 0 and code == 0 and 'PASS vfs_stateful_host_fuzz_100000' in text and 'FAIL' not in text
    with (out / 'record-locks.log').open('w') as log:
        locks_run = subprocess.run([sys.executable, 'tests/host_record_locks.py'], cwd=ROOT,
                                   stdout=log, stderr=subprocess.STDOUT)
    locks_text = (out / 'record-locks.log').read_text()
    locks_report = None
    locks_result = None
    if locks_text.splitlines():
        candidate = Path(locks_text.splitlines()[-1])
        if candidate.is_file() and candidate.name == 'results.json':
            locks_report = str(candidate)
            locks_result = json.loads(candidate.read_text())
    locks_good = locks_run.returncode == 0 and locks_result is not None and locks_result.get('passed') is True
    inputs = ['tests/host_vfs.c', 'servers/vfs/server.c', 'servers/vfs/pipe_wait.h',
              'servers/vfs/archive.h', 'servers/vfs/regression.c', 'servers/drivers/devices.h',
              'servers/vfs/metadata.h', 'servers/vfs/path_io.h', 'servers/vfs/record_lock.h',
              'servers/vfs/file_data.h',
              'include/nos/abi.h', 'include/nos/user.h', 'include/nos/poll_abi.h']
    result = {'passed': good and locks_good, 'bounds_passed': good,
              'record_locks': {'passed': locks_good, 'returncode': locks_run.returncode,
                               'report': locks_report},
              'scope': 'VFS bounds and record-lock state; host RPC/device stubs do not verify kernel IPC or hardware',
              'build_returncode': build.returncode, 'returncode': code, 'iterations': 100000,
              'fuzz_seed': 123456789, 'operation_range': [1, 44], 'descriptor_argument_range': [-4, 35],
              'compiler': subprocess.check_output(['gcc', '--version'], text=True).splitlines()[0],
              'source_sha256': {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in inputs},
              'sanitizers': ['address', 'undefined']}
    (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))
    if not good:
        print((out / 'build.log').read_text()[-5000:] if build.returncode else text[-5000:])
    if not locks_good:
        print(locks_text[-5000:])
    print(out / 'results.json')
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
