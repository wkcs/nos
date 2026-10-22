#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Actual VFS multi-batch process GC with mock IPC."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
REQUIRED = {
    'vfs_process_gc_first_middle_last_failures_preserve_state',
    'vfs_process_gc_batches_cover_203_full_generation_owners',
    'vfs_process_gc_invalid_last_state_preserves_state',
    'vfs_process_gc_late_liveness_generation_and_fork_pins',
    'vfs_process_gc_6576_ofd_references_reclaim_exactly',
}

INPUTS = [
    'tests/host_process_vfs.c', 'tests/host_record_locks.c', 'servers/vfs/server.c', 'servers/vfs/record_lock.h',
    'servers/vfs/pipe_wait.h', 'servers/vfs/metadata.h', 'servers/vfs/archive.h',
    'servers/vfs/path_io.h', 'servers/vfs/path_storage.h', 'servers/vfs/permissions.h', 'servers/vfs/file_data.h', 'servers/drivers/devices.h',
    'servers/fatfs/adapter.h', 'include/nos/abi.h', 'include/nos/poll_abi.h',
]


def main():
    out = ROOT / 'out/microkernel/host-process-vfs' / str(time.time_ns())
    out.mkdir(parents=True)
    hashes = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in INPUTS}
    command = ['gcc', '-DNOS_MMU', '-DNOS_LINUX_COMPAT', '-Iinclude/nos',
               '-Wall', '-Wextra', '-Werror', '-std=gnu11', '-O1', '-g',
               '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               'tests/host_process_vfs.c', '-o', str(out / 'host-process-vfs')]
    with (out / 'build.log').open('w') as stream:
        build = subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT)
    code = None
    if not build.returncode:
        environment = os.environ.copy()
        environment.update(ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
                           UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        with (out / 'host.log').open('w') as stream:
            try:
                run = subprocess.run([str(out / 'host-process-vfs')], cwd=ROOT,
                                     env=environment, stdout=stream,
                                     stderr=subprocess.STDOUT, timeout=120)
                code = run.returncode
            except subprocess.TimeoutExpired:
                pass
    text = (out / 'host.log').read_text() if (out / 'host.log').exists() else ''
    checks = {line[5:] for line in text.splitlines() if line.startswith('PASS ')}
    stable = hashes == {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in INPUTS}
    good = build.returncode == 0 and code == 0 and stable and REQUIRED <= checks and 'FAIL' not in text
    result = {'passed': good, 'scope': 'Actual VFS multi-batch GC, full generation IDs, pending-fork pins and OFD references; mocked IPC/liveness are not kernel or QEMU validation',
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
