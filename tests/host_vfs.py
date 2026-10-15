#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Sanitized VFS bounds tests with stubbed transport."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    out = ROOT / 'out/microkernel/host-vfs' / str(time.time_ns())
    out.mkdir(parents=True)
    binary = out / 'host-vfs'
    command = ['gcc', '-DNOS_MMU', '-Iinclude/nos', '-fsanitize=address,undefined',
               '-fno-omit-frame-pointer', '-g', '-O1', 'tests/host_vfs.c', '-o', str(binary)]
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
    inputs = ['tests/host_vfs.c', 'servers/vfs/server.c', 'servers/vfs/pipe_wait.h', 'servers/vfs/regression.c', 'include/nos/abi.h']
    result = {'passed': good, 'scope': 'VFS bounds only; host RPC/device stubs do not verify IPC or hardware',
              'build_returncode': build.returncode, 'returncode': code, 'iterations': 100000,
              'compiler': subprocess.check_output(['gcc', '--version'], text=True).splitlines()[0],
              'source_sha256': {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in inputs},
              'sanitizers': ['address', 'undefined']}
    (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))
    if not good:
        print((out / 'build.log').read_text()[-5000:] if build.returncode else text[-5000:])
    print(out / 'results.json')
    return 0 if good else 1


if __name__ == '__main__':
    raise SystemExit(main())
