#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Actual VFS record locks with mock IPC."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
REQUIRED = {
    'vfs_pending_owner_gc_lost_reader_exit_unpins_epipe',
    'vfs_pending_owner_gc_lost_writer_exit_preserves_data_and_eof',
    'vfs_pending_owner_gc_full_generation_and_live_thread_isolation',
    'vfs_bulk_pread_ram_inline_and_offset_preserved',
    'vfs_bulk_pread_archive_short_read_and_eof',
    'vfs_bulk_pread_store_failure_and_cancel_preserve_state',
    'vfs_bulk_pread_format_permissions_and_bounds',
    'record_locks_process_conflicts_and_getlk',
    'record_locks_batch_query_failure_retains_entire_state',
    'record_locks_split_merge_negative_len_checked_offsets',
    'record_locks_any_close_and_dup2_target_release',
    'record_locks_fork_no_lock_inheritance_exec_cloexec',
    'record_locks_wait_deadlock_and_unlock_wakeup',
    'record_locks_cancel_reclaims_pin_without_ghost_lock',
    'record_locks_close_fd_reuse_cannot_redirect_waiter',
    'record_locks_failed_reply_does_not_commit',
    'record_locks_enolck_split_rollback_owner_exit',
    'record_locks_group_gc_lost_exit_wakes_waiter',
    'record_locks_group_gc_query_error_and_full_generation',
    'record_locks_group_gc_fork_pins_and_dead_waiter',
    'record_locks_group_gc_last_fork_ack_pipe_eof',
    'record_locks_group_gc_full_live_table_fork_inheritance',
    'record_locks_fork_table_exhaustion_preserves_parent',
}
INPUTS = [
    'tests/host_record_locks.c', 'servers/vfs/server.c', 'servers/vfs/record_lock.h',
    'servers/vfs/pipe_wait.h', 'servers/vfs/metadata.h', 'servers/vfs/archive.h',
    'servers/vfs/path_io.h', 'servers/vfs/path_storage.h', 'servers/vfs/permissions.h', 'servers/vfs/file_data.h', 'servers/drivers/devices.h',
    'servers/fatfs/adapter.h', 'include/nos/abi.h', 'include/nos/poll_abi.h',
]


def main():
    out = ROOT / 'out/microkernel/host-record-locks' / str(time.time_ns())
    out.mkdir(parents=True)
    hashes = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in INPUTS}
    command = ['gcc', '-DNOS_MMU', '-DNOS_LINUX_COMPAT', '-Iinclude/nos',
               '-Wall', '-Wextra', '-Werror', '-std=gnu11', '-O1', '-g',
               '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               'tests/host_record_locks.c', '-o', str(out / 'host-record-locks')]
    with (out / 'build.log').open('w') as stream:
        build = subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT)
    code = None
    if not build.returncode:
        environment = os.environ.copy()
        environment.update(ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
                           UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        with (out / 'host.log').open('w') as stream:
            try:
                run = subprocess.run([str(out / 'host-record-locks')], cwd=ROOT,
                                     env=environment, stdout=stream,
                                     stderr=subprocess.STDOUT, timeout=120)
                code = run.returncode
            except subprocess.TimeoutExpired:
                pass
    text = (out / 'host.log').read_text() if (out / 'host.log').exists() else ''
    checks = {line[5:] for line in text.splitlines() if line.startswith('PASS ')}
    stable = hashes == {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in INPUTS}
    good = build.returncode == 0 and code == 0 and stable and REQUIRED <= checks and 'FAIL' not in text
    result = {'passed': good, 'scope': 'Actual VFS lock implementation; mock IPC is not kernel or QEMU validation',
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
