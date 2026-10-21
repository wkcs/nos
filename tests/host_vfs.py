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


def run_sanitizer(script, log_path):
    with log_path.open('w') as log:
        run = subprocess.run([sys.executable, script], cwd=ROOT,
                             stdout=log, stderr=subprocess.STDOUT)
    text = log_path.read_text()
    report = None
    result = None
    if text.splitlines():
        candidate = Path(text.splitlines()[-1])
        if candidate.is_file() and candidate.name == 'results.json':
            report = str(candidate)
            try:
                result = json.loads(candidate.read_text())
            except json.JSONDecodeError:
                pass
    passed = run.returncode == 0 and result is not None and result.get('passed') is True
    return {'passed': passed, 'returncode': run.returncode, 'report': report}, text


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
    locks, locks_text = run_sanitizer('tests/host_record_locks.py', out / 'record-locks.log')
    pipes, pipes_text = run_sanitizer('tests/host_pipe_atomic.py', out / 'pipe-atomic.log')
    credentials, credentials_text = run_sanitizer('tests/host_credentials_vfs.py', out / 'credentials.log')
    resources, resources_text = run_sanitizer('tests/host_resources_vfs.py', out / 'resources.log')
    paths, paths_text = run_sanitizer('tests/host_path4096_vfs.py', out / 'path4096.log')
    inputs = ['tests/host_vfs.c', 'servers/vfs/server.c', 'servers/vfs/pipe_wait.h',
              'servers/vfs/archive.h', 'servers/vfs/regression.c', 'servers/drivers/devices.h',
              'servers/vfs/metadata.h', 'servers/vfs/path_io.h', 'servers/vfs/path_storage.h', 'servers/vfs/record_lock.h',
              'servers/vfs/file_data.h', 'servers/vfs/permissions.h',
              'include/nos/abi.h', 'include/nos/user.h', 'include/nos/poll_abi.h']
    result = {'passed': good and locks['passed'] and pipes['passed'] and credentials['passed'] and resources['passed'] and paths['passed'], 'bounds_passed': good,
              'record_locks': locks, 'pipe_atomic': pipes, 'credentials': credentials, 'resources': resources, 'paths': paths,
              'scope': 'VFS bounds, record-lock, pipe, permission, resource admission and PATH4096 state; host RPC/device stubs do not verify kernel IPC or hardware',
              'build_returncode': build.returncode, 'returncode': code, 'iterations': 100000,
              'fuzz_seed': 123456789, 'operation_range': [1, 44], 'descriptor_argument_range': [-4, 35],
              'compiler': subprocess.check_output(['gcc', '--version'], text=True).splitlines()[0],
              'source_sha256': {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in inputs},
              'sanitizers': ['address', 'undefined']}
    (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))
    if not good:
        print((out / 'build.log').read_text()[-5000:] if build.returncode else text[-5000:])
    if not locks['passed']:
        print(locks_text[-5000:])
    if not pipes['passed']:
        print(pipes_text[-5000:])
    if not credentials['passed']:
        print(credentials_text[-5000:])
    if not resources['passed']:
        print(resources_text[-5000:])
    if not paths['passed']:
        print(paths_text[-5000:])
    print(out / 'results.json')
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
