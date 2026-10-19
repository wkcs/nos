#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Genuine Debian ld.so/glibc guest checks."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

from qemu_matrix import ROOT, COMMON, MMU, file_digest, git_identity, tool_version
from qemu_guest import run_guest
from qemu_platforms import qemu_command

PLATFORMS = ('arm_virt', 'arm64_virt', 'riscv_virt')
RNG = ['-object', 'rng-random,id=nosrng,filename=/dev/urandom',
       '-device', 'virtio-rng-device,rng=nosrng']
LINUX = {'linux_exec_requires_entropy', 'linux_dynamic_interpreter', 'glibc_dynamic_application',
         'linux_dynamic_needed_constructor', 'glibc_real_stdio_malloc_errno_tls_dlopen'}
LINUX |= {'glibc_pthread_default_create', 'glibc_cond_broadcast_tls_errno_malloc_stdio',
          'glibc_pthread_detach_tsd_exit', 'glibc_pthread_robust_owner_death',
          'glibc_pthread_cancel_condition_cleanup', 'glibc_pthread_cancel_async_cleanup',
          'glibc_nptl_threads', 'glibc_poll_select_readiness', 'glibc_poll_timeout_eof_invalid'}
LINUX |= {'glibc_signal_handler_sigreturn', 'glibc_signal_mask_pending_queue',
          'glibc_signal_alternate_stack', 'glibc_signal_realtime_fifo_wait',
          'glibc_signal_pipe_ignore', 'glibc_posix_signals',
          'glibc_poll_application', 'glibc_signal_application'}
LINUX |= {'glibc_fork_private_memory_shared_ofd', 'glibc_fork_signal_interrupt_nanosleep',
          'glibc_waitpid_nonblocking_status', 'glibc_execve_pid_fd_cloexec', 'glibc_posix_process',
          'glibc_process_application'}
LINUX |= {'glibc_ppoll_pselect_atomic_sigmask'}
LINUX |= {'glibc_signal_realtime_interval_timer'}
LINUX |= {'glibc_rwlock_barrier_once', 'glibc_tsd_destructor_reentry',
          'glibc_unnamed_semaphore_wait_post', 'glibc_unnamed_semaphore_timedwait', 'glibc_sync_application'}
LINUX |= {'glibc_file_modes_positioned_io_truncate', 'glibc_file_links_dirfd_lifetime', 'glibc_files_application'}
LINUX |= {'glibc_path_255_double_path_cwd', 'glibc_directory_long_fragmented_record'}
LINUX |= {'glibc_pread_bulk_contents_guard_offset', 'glibc_pread_bulk_archive_inline_equivalence'}
LINUX |= {'glibc_path_bulk_guard_and_pairs', 'glibc_path_getcwd_getdents_280', 'glibc_paths_application'}
LINUX |= {'glibc_exec_argument_budget', 'glibc_exec_many_short_arguments'}
LINUX |= {'glibc_exec_guard_terminated_argument'}
LINUX |= {'glibc_shared_futex_alias_requeue'}
LINUX |= {'glibc_file_shared_coherence_permissions', 'glibc_shm_process_shared_sem_different_va',
          'glibc_named_sem_exec_unlink_recreate', 'glibc_futex_shared_application', 'glibc_shared_sync_application'}
LINUX |= {'glibc_execve_failed_preserves_image', 'glibc_signal_restart_blocking_read',
          'glibc_signal_default_termination_status', 'glibc_execve_nonleader_thread'}
LINUX |= {'glibc_time_gettimeofday_uname', 'glibc_process_session_group_signals', 'glibc_execve_relative_symlink'}
LINUX_KERNEL = {'ipc_bulk_token_bounds', 'ipc_bulk_cancel_revocation'}
LINUX_KERNEL |= {'group_query_capability', 'group_query_dead_zombie',
                 'group_query_leader_exit_thread_live', 'group_query_generation_slot_reuse',
                 'group_query_observer_cleanup', 'group_query_batch_snapshot'}
LINUX_KERNEL |= {'ipc_bulk_4096_fixture', 'ipc_bulk_4096_pages_canaries',
                 'ipc_bulk_4096_limits_zero', 'ipc_bulk_4096_permissions_revocation'}
LINUX_GC_KERNEL = {'record_lock_exit_slot_reuse_gc', 'record_lock_pending_fork_fd_inheritance'}
LINUX_SINGLE_CPU_KERNEL = {'pipe_pending_owner_exit_slot_reuse'}
# Native regression supplies the helper's owner/observer modes; it is not a standalone case.
LINUX_HELPER_APPS = ('glibc_record_gc',)
APP_CHECKS = {
    'dynamic_check': {'linux_dynamic_needed_constructor'},
    'glibc_check': {'glibc_real_stdio_malloc_errno_tls_dlopen', 'glibc_exec_argument_budget',
                     'glibc_exec_many_short_arguments', 'glibc_exec_guard_terminated_argument'},
    'glibc_threads': {'glibc_pthread_default_create', 'glibc_cond_broadcast_tls_errno_malloc_stdio',
                      'glibc_pthread_detach_tsd_exit', 'glibc_pthread_robust_owner_death',
                      'glibc_pthread_cancel_condition_cleanup', 'glibc_pthread_cancel_async_cleanup'},
    'glibc_poll': {'glibc_poll_select_readiness', 'glibc_poll_timeout_eof_invalid',
                   'glibc_ppoll_pselect_atomic_sigmask'},
    'glibc_signals': {'glibc_signal_handler_sigreturn', 'glibc_signal_mask_pending_queue',
                      'glibc_signal_alternate_stack', 'glibc_signal_realtime_fifo_wait',
                      'glibc_signal_pipe_ignore', 'glibc_posix_signals', 'glibc_signal_realtime_interval_timer'},
    'glibc_process': {'glibc_fork_private_memory_shared_ofd', 'glibc_fork_signal_interrupt_nanosleep',
                      'glibc_waitpid_nonblocking_status', 'glibc_execve_pid_fd_cloexec', 'glibc_posix_process',
                      'glibc_execve_failed_preserves_image', 'glibc_signal_restart_blocking_read',
                      'glibc_signal_default_termination_status', 'glibc_execve_nonleader_thread',
                      'glibc_time_gettimeofday_uname', 'glibc_process_session_group_signals', 'glibc_execve_relative_symlink'},
    'glibc_sync': {'glibc_rwlock_barrier_once', 'glibc_tsd_destructor_reentry',
                   'glibc_unnamed_semaphore_wait_post', 'glibc_unnamed_semaphore_timedwait'},
    'glibc_files': {'glibc_file_modes_positioned_io_truncate', 'glibc_file_links_dirfd_lifetime',
                    'glibc_path_255_double_path_cwd', 'glibc_directory_long_fragmented_record',
                    'glibc_pread_bulk_contents_guard_offset', 'glibc_pread_bulk_archive_inline_equivalence'},
    'glibc_futex_shared': {'glibc_shared_futex_alias_requeue'},
    'glibc_shared_sync': {'glibc_file_shared_coherence_permissions', 'glibc_shm_process_shared_sem_different_va',
                         'glibc_named_sem_exec_unlink_recreate'},
    'glibc_paths': {'glibc_path_bulk_guard_and_pairs', 'glibc_path_getcwd_getdents_280'},
}
RECORD_LOCK_CHECKS = {
    'glibc_record_lock_process_ownership',
    'glibc_record_lock_range_normalization',
    'glibc_record_lock_split_merge',
    'glibc_record_lock_close_dup2_release',
    'glibc_record_lock_buffer_access',
    'glibc_record_lock_wait_wake_owner_exit',
    'glibc_record_lock_wait_signal_eintr',
    'glibc_record_lock_wait_signal_restart',
    'glibc_record_lock_deadlock_detection',
    'glibc_record_lock_waiter_exit_reclaims',
    'glibc_record_lock_wait_cancel_reclaims',
    'glibc_record_lock_exec_cloexec_release',
}
LINUX |= RECORD_LOCK_CHECKS | {'glibc_record_locks_application'}
APP_CHECKS['glibc_record_locks'] = RECORD_LOCK_CHECKS
RECORD_LIFECYCLE_CHECKS = {
    'glibc_record_exec_preserves_and_failed_exec',
    'glibc_record_exec_cloexec_inode_scope',
    'glibc_record_alias_unlink_recreate',
    'glibc_record_inode_reuse_cleanup',
    'glibc_record_rename_overwrite_inode_scope',
    'glibc_record_dup2_identity_and_failed_source',
    'glibc_record_dup2_same_ofd_target_close',
}
LINUX |= RECORD_LIFECYCLE_CHECKS | {'glibc_record_lock_lifecycle_application'}
APP_CHECKS['glibc_record_lock_lifecycle'] = RECORD_LIFECYCLE_CHECKS
PIPE_ATOMIC_CHECKS = {
    'glibc_pipe_limits_4096_iov1024',
    'glibc_pipe_atomic_process_records',
    'glibc_pipe_atomic_nonblock_no_partial',
    'glibc_pipe_atomic_blocked_wakeup',
    'glibc_pipe_atomic_signal_eintr_no_ghost',
    'glibc_pipe_atomic_cancel_no_ghost',
    'glibc_pipe_ofd_close_reuse_pin',
    'glibc_pipe_large_blocking_progress',
    'glibc_pipe_large_nonblock_prefix',
    'glibc_pipe_partial_epipe_sigpipe_prefix',
    'glibc_pipe_iov1024_guard_precheck',
}
LINUX |= PIPE_ATOMIC_CHECKS | {'glibc_pipe_atomic_application'}
APP_CHECKS['glibc_pipe_atomic'] = PIPE_ATOMIC_CHECKS
POSIX_VFS = {'page_allocator', 'mapping_transaction_rollback', 'timer_preemption', 'vfs_archive_large_readonly',
             'vfs_openat_cwd_stat', 'vfs_archive_getdents_fragment', 'vfs_positional_io_offsets',
             'vfs_descriptor_cloexec', 'vfs_getrandom_device', 'vfs_mode_umask_truncate',
             'vfs_hardlink_symlink_identity', 'vfs_dirfd_rename_unlink'}
SHARED_FILES = {'page_allocator', 'mapping_transaction_rollback', 'timer_preemption', 'vfs_shared_file_same_pages_unlink',
                'vfs_shared_file_readonly_grant'}
LONG_PATHS = {'page_allocator', 'mapping_transaction_rollback', 'timer_preemption',
              'vfs_long_path_openat_archive', 'vfs_long_path_pair_cwd_resolve',
              'vfs_long_dirent_boundary', 'vfs_long_path_thread_isolation'}


def checked_artifacts(manifest, platform):
    identity = manifest['platforms'][platform]
    artifacts = list(identity['libraries'].values()) + [identity['compiler']]
    artifacts += manifest['glibc_source']['artifacts']
    for artifact in artifacts:
        path = Path(artifact.get('file', artifact.get('path')))
        if file_digest(path) != artifact['sha256']:
            raise ValueError(f'Artifact SHA256 mismatch: {path}')
    return identity


def main(mode='glibc'):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', action='append', choices=PLATFORMS)
    parser.add_argument('--manifest', type=Path, default=Path('/workspace/.nos-env/glibc/manifest.json'))
    parser.add_argument('--output-root', type=Path)
    parser.add_argument('--qemu-bindir', default=os.environ.get('QEMU_BINDIR', ''))
    parser.add_argument('--timeout', type=int, help='Host guest deadline in seconds; default 420 for full glibc, 180 for focused/native modes')
    parser.add_argument('--cpus', type=int, choices=(1, 2, 4), default=1)
    parser.add_argument('--app', choices=APP_CHECKS, help='Development-only single application; default tests all applications')
    args = parser.parse_args()
    if args.app and mode != 'glibc':
        parser.error('--app is only available for the glibc profile')
    if args.timeout is not None and args.timeout <= 0:
        parser.error('--timeout must be positive')
    if args.timeout is not None:
        timeout_seconds, timeout_policy = args.timeout, 'explicit'
    elif mode == 'glibc' and args.app is None:
        timeout_seconds, timeout_policy = 420, 'full_glibc'
    else:
        timeout_seconds, timeout_policy = 180, 'focused_or_native'
    run_id = time.time_ns()
    report = ROOT / 'out/microkernel' / (mode + '-reports') / str(run_id)
    report.mkdir(parents=True)
    linux_profile = mode in ('glibc', 'posix-shared', 'posix-long-path')
    uses_rng = mode in ('glibc', 'posix-vfs')
    metadata = {'run_id': run_id, 'profile': mode, 'source': git_identity(),
                'started_ns': time.time_ns(), 'completed': False,
                'focused_application': args.app,
                'timeout_seconds': timeout_seconds, 'timeout_policy': timeout_policy,
                'full_posix_conformance': False,
                'entropy_backend': '/dev/urandom via QEMU virtio-rng; nondeterministic' if uses_rng else None}
    (report / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
    manifest = json.loads(args.manifest.read_text()) if linux_profile else None
    if manifest:
        shutil.copy2(args.manifest, report / 'glibc-manifest.json')
        metadata['manifest_sha256'] = file_digest(args.manifest)
    results = []
    for platform in args.platform or PLATFORMS:
        default_out = ROOT / 'out/microkernel' / (mode + '-build')
        if args.app:
            default_out = default_out / 'focused' / args.app
        out = ((args.output_root or default_out) / platform).resolve()
        saved = report / platform
        saved.mkdir()
        out.mkdir(parents=True, exist_ok=True)
        item = {'platform': platform, 'passed': False,
                'timeout_seconds': timeout_seconds, 'timeout_policy': timeout_policy}
        results.append(item)
        identity = checked_artifacts(manifest, platform) if manifest else None
        if identity:
            item['glibc'] = identity
            item['glibc_source'] = manifest['glibc_source']
        build_command = ['make', '-j5', f'PLATFORM={platform}', 'LIBC=minimal',
                         'BUILD_MODE=selftest', f'O={out}', f'SMP={args.cpus}',
                         'LINUX_COMPAT=1' if linux_profile else 'PERSISTENCE_TEST=7']
        if mode == 'posix-shared':
            build_command += ['PERSISTENCE_TEST=8']
        if mode == 'posix-long-path':
            build_command += ['PERSISTENCE_TEST=9']
        if args.app:
            build_command += ['EXTRA_CFLAGS=-DNOS_LINUX_FOCUSED_APP=\\"/bin/' + args.app + '\\"']
        item['build_command'] = build_command
        with (saved / 'build.log').open('w') as stream:
            build = subprocess.run(build_command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT)
        item['build_returncode'] = build.returncode
        if build.returncode:
            print((saved / 'build.log').read_text()[-5000:])
            continue
        shutil.copy2(out / 'build-config.json', saved / 'build-config.json')
        command = qemu_command(platform, out / 'nos.elf', args.qemu_bindir, args.cpus,
                               extra_args=RNG if uses_rng else ())
        item.update(command=command, image_sha256=file_digest(out / 'nos.elf'),
                    cpus=args.cpus, qemu_version=tool_version(command[0]))
        if identity:
            item['apps'] = {}
            readelf = identity['triple'] + '-readelf'
            for name in ['nos-linux-gateway.elf', *(app + '.elf' for app in (*APP_CHECKS, *LINUX_HELPER_APPS)), 'libnos_probe.so']:
                elf = out / 'apps' / name
                inspection = subprocess.check_output([readelf, '-lWd', str(elf)], text=True)
                (saved / (name + '.readelf')).write_text(inspection)
                item['apps'][name] = {'sha256': file_digest(elf), 'bytes': elf.stat().st_size}
                if name.removesuffix('.elf') in (*APP_CHECKS, *LINUX_HELPER_APPS) and identity['interpreter'] not in inspection:
                    raise ValueError(f'Wrong interpreter: {elf}')
        host_started = time.time()
        try:
            text, code, metrics = run_guest(command, saved / 'qemu.log', timeout_seconds)
        except OSError as exc:
            text, code, metrics = str(exc), None, {'error': str(exc)}
        host_finished = time.time()
        realtime = [int(value) for value in re.findall(r'^NOS REALTIME (\d+)\r?$', text, re.MULTILINE)]
        check_realtime = mode == 'glibc' and args.app in (None, 'glibc_check')
        realtime_ok = not check_realtime or (len(realtime) == 1 and
                                         host_started - 10 <= realtime[0] <= host_finished + 10)
        pthread_masks = [int(value, 16) for value in re.findall(r'^glibc_pthread_cpu_mask=([0-9a-f]+)\r?$', text, re.MULTILINE)]
        check_threads = mode == 'glibc' and args.app in (None, 'glibc_threads')
        pthread_cpus_ok = not check_threads or (len(pthread_masks) == 1 and pthread_masks[0] and
                          not (pthread_masks[0] & ~((1 << args.cpus) - 1)) and
                          (args.cpus == 1 or bool(pthread_masks[0] & (pthread_masks[0] - 1))))
        checks = {line[5:] for line in text.splitlines() if line.startswith('PASS ')}
        linux_required = LINUX if not args.app else APP_CHECKS[args.app] | {'linux_focused_application'}
        native_required = {'posix-shared': SHARED_FILES, 'posix-long-path': LONG_PATHS, 'posix-vfs': POSIX_VFS}
        required = native_required[mode] if mode != 'glibc' else COMMON | MMU | LINUX_KERNEL | LINUX_GC_KERNEL | linux_required | {
            'fpu_context_switch', 'shared_thread_create', 'shared_thread_lock', 'shared_thread_join_futex_condition'}
        if mode in ('posix-shared', 'posix-long-path'):
            required |= LINUX_KERNEL
        if mode == 'glibc' and args.cpus == 1:
            required |= LINUX_SINGLE_CPU_KERNEL
        if mode == 'glibc' and platform.startswith('riscv'):
            required |= {'riscv_halfword_thread_entry'}
        if mode == 'glibc' and platform == 'arm_virt':
            required |= {'arm_linux_kuser_helpers'}
            if args.app in (None, 'glibc_record_locks'):
                required |= {'glibc_record_lock_arm_old_wide_layout'}
        if mode == 'glibc' and platform != 'arm_virt':
            required |= {'native_width_handle_validation', 'capability_high_bits_validation'}
        if mode == 'glibc' and args.cpus > 1:
            required |= {'smp_parallel_shared_threads', 'smp_tlb_shootdown', 'smp_clock_rate'}
        missing = sorted(required - checks)
        clock_ok = mode != 'glibc' or args.cpus == 1 or (
            metrics.get('host_clock_seconds') is not None and metrics['host_clock_seconds'] >= 0.4)
        item.update(exit_code=code, runtime_metrics=metrics, required=sorted(required),
                    checks=sorted(checks), missing=missing,
                    realtime_guest_seconds=realtime, realtime_host_window=[host_started, host_finished],
                    realtime_verified=realtime_ok if check_realtime else None,
                    glibc_pthread_cpu_masks=pthread_masks,
                    glibc_pthread_cross_cpu_verified=pthread_cpus_ok if check_threads else None,
                    clock_rate_verified=clock_ok if args.cpus > 1 and mode == 'glibc' else None,
                    passed=code == 0 and 'NOS TEST PASS' in text and 'FAIL' not in text and not missing and clock_ok and realtime_ok and pthread_cpus_ok)
        if manifest:
            checked_artifacts(manifest, platform)
        print(f'{mode}/{platform}: {"PASS" if item["passed"] else "FAIL"} ({len(checks)} checks, exit={code})', flush=True)
        if not item['passed']:
            print(text[-6000:], '\nMissing:', missing, flush=True)
    metadata['source_after'] = git_identity()
    metadata['source_changed_during_run'] = metadata['source'] != metadata['source_after']
    metadata.update(completed=True, finished_ns=time.time_ns())
    (report / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
    (report / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print(report / 'results.json', flush=True)
    return 0 if results and all(row['passed'] for row in results) else 1


if __name__ == '__main__':
    sys.exit(main())
