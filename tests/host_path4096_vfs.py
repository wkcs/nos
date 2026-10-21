#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Actual VFS PATH4096 namespace with mock IPC."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
REQUIRED = {
    'vfs_path4096_exact_boundary_and_name255',
    'vfs_path4096_two_read_spans_8192_atomic_namespace',
    'vfs_path4096_second_fetch_error_cancel_preserves_namespace',
    'vfs_path4096_long_symlink_backing_readlink_truncation_cleanup',
    'vfs_path4096_pinned_cwd_tracks_ancestor_rename',
    'vfs_path4096_unlinked_cwd_not_retargeted_by_replacement',
    'vfs_path4096_persistent_path64_boundary_unchanged',
    'vfs_path4096_bounded_scratch_reclaimed',
}

INPUTS = [
    'tests/host_path4096_vfs.c', 'tests/host_record_locks.c', 'servers/vfs/server.c', 'servers/vfs/record_lock.h',
    'servers/vfs/pipe_wait.h', 'servers/vfs/metadata.h', 'servers/vfs/archive.h',
    'servers/vfs/path_io.h', 'servers/vfs/path_storage.h', 'servers/vfs/permissions.h', 'servers/vfs/file_data.h', 'servers/drivers/devices.h',
    'servers/fatfs/adapter.h', 'include/nos/abi.h', 'include/nos/poll_abi.h',
]


def main():
    out = ROOT / 'out/microkernel/host-path4096-vfs' / str(time.time_ns())
    out.mkdir(parents=True)
    hashes = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in INPUTS}
    command = ['gcc', '-DNOS_MMU', '-DNOS_LINUX_COMPAT', '-Iinclude/nos',
               '-Wall', '-Wextra', '-Werror', '-std=gnu11', '-O1', '-g',
               '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               'tests/host_path4096_vfs.c', '-o', str(out / 'host-path4096-vfs')]
    with (out / 'build.log').open('w') as stream:
        build = subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT)
    code = None
    if not build.returncode:
        environment = os.environ.copy()
        environment.update(ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
                           UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        with (out / 'host.log').open('w') as stream:
            try:
                run = subprocess.run([str(out / 'host-path4096-vfs')], cwd=ROOT,
                                     env=environment, stdout=stream,
                                     stderr=subprocess.STDOUT, timeout=120)
                code = run.returncode
            except subprocess.TimeoutExpired:
                pass
    text = (out / 'host.log').read_text() if (out / 'host.log').exists() else ''
    checks = {line[5:] for line in text.splitlines() if line.startswith('PASS ')}
    metrics = {}
    for line in text.splitlines():
        if line.startswith('METRIC '):
            metrics.update({key: int(value) for key, value in
                            (item.split('=', 1) for item in line[7:].split())})
    stable = hashes == {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in INPUTS}
    good = build.returncode == 0 and code == 0 and stable and REQUIRED <= checks and 'FAIL' not in text
    result = {'passed': good, 'scope': 'Actual VFS namespace and paths; mocked descriptors/credentials/RTC/IPC are not kernel or QEMU validation',
              'build_command': command, 'build_returncode': build.returncode,
              'returncode': code, 'required': sorted(REQUIRED), 'missing': sorted(REQUIRED - checks),
              'source_stable': stable, 'source_sha256': hashes,
              'runtime_metrics': metrics,
              'sanitizers': ['address', 'undefined']}
    (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))
    if not good:
        print((out / 'build.log').read_text()[-5000:] if build.returncode else text[-5000:])
    print(out / 'results.json')
    return 0 if good else 1


if __name__ == '__main__':
    raise SystemExit(main())
