#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Real guest capability validation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import shutil
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from qemu_platforms import PLATFORMS, BASELINE_PLATFORMS, PLATFORM_LIBRARIES, qemu_command
from build_signature import atomic_write_changed
from qemu_guest import run_guest
COMMON = {
    'page_allocator', 'user_syscall', 'unknown_syscall', 'user_pointer_validation',
    'ipc_capabilities', 'vfs_mkdir', 'vfs_rpc_read_write_seek', 'vfs_stat',
    'vfs_fd_ownership', 'vfs_close_unlink_errors', 'vfs_mount_procfs',
    'vfs_path_permissions', 'anonymous_memory', 'wx_mapping_rejected',
    'memory_unmap', 'heap_bounds', 'scheduler_sleep_wait',
    'mutex_blocking_priority_inheritance', 'mutex_stale_handle', 'timer_preemption',
    'vfs_process_cleanup', 'multi_page_mapping', 'process_stale_handle',
    'ipc_lifecycle_authentication', 'mapping_quota_recovery',
    'tls_pointer_validation',
    'abi_version_contract', 'resource_lifecycle_1000',
    'vfs_emfile_transaction', 'vfs_sparse_zero', 'vfs_unlink_open',
    'vfs_path_normalization', 'vfs_dup_shared_offset', 'vfs_pipe', 'vfs_rename_readdir',
    'vfs_resource_cycles', 'device_capability',
    'versioned_syscall_abi', 'multiple_endpoint_rpc', 'ipc_timeout_stale_reply',
    'endpoint_capability_revocation', 'endpoint_exit_reclamation',
    'transitive_priority_inheritance', 'vfs_service_restart',
    'multi_lock_priority_inheritance', 'orphan_task_reclamation',
    'safe_io_open', 'safe_io_bad_pointers', 'zero_io_bad_descriptor', 'stdio_long_write',
    'elf_object_baseline', 'elf_independent_app', 'elf_malformed_rejected',
    'elf_arguments_validated', 'elf_missing_image', 'elf_failed_load_reclaims_pages',
    'mapping_transaction_rollback',
    'ipc_reserved_cancel_validation', 'endpoint_receive_right_revocation',
    'shared_thread_lazy_mutex_capability', 'ipc_invalid_reply_buffer_completion',
    'vfs_pipe_blocking_read', 'vfs_pipe_blocking_write', 'vfs_pipe_nonblocking',
    'vfs_pipe_timeout_cancel_reclaims',
    'mixed_ipc_mutex_deadlock_detection', 'endpoint_1000_lifecycle_cycles',
    'libc_clock_64bit_conversion',
    'futex_readonly_atomic_load',
    'elf_entry_file_backed', 'elf_segment_boundaries',
}
MMU = {'mmu_page_protection', 'mmu_kernel_isolation', 'mmu_write_fault_recovery', 'service_survives_client_fault', 'private_address_spaces', 'mmu_execute_never', 'tls_mapping_revocation'}
MMU |= {'user_stack_guard', 'partial_vma_operations', 'prot_none_mapping'}
MMU |= {'elf_rodata_execute_never'}
NEWLIB = {'newlib_malloc', 'newlib_printf', 'newlib_stdio_vfs'}
NEWLIB |= {'newlib_pthread_default_create', 'newlib_pthread_mutex',
           'newlib_pthread_join_condition_tls_malloc', 'newlib_pthread_shared_descriptors',
           'newlib_cancel_explicit_unsupported', 'newlib_pthread_shared_stream_open', 'newlib_pthread_shared_stdio'}
MUSL = {'musl_malloc', 'musl_printf', 'musl_stdio_vfs', 'musl_large_allocation'}
MUSL |= {'musl_zero_vm_length', 'musl_zero_vm_preserves_mapping', 'musl_pthread_default_create',
         'musl_pthread_mutex', 'musl_pthread_join_condition_tls_malloc', 'musl_cancel_explicit_unsupported',
         'musl_pthread_shared_descriptors'}
MUSL |= {'musl_pthread_shared_stream_open', 'musl_pthread_shared_stdio', 'musl_signed_futex_setup',
         'musl_signed_futex_wait_wake', 'musl_futex_atomic_requeue', 'musl_condvar_three_waiter_broadcast'}

def file_digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def libc_identity(platform, libc, configuration):
    if libc == 'minimal':
        return {'name': 'minimal'}
    library = configuration['configuration']['libraries']
    archive = Path(library)
    if not archive.is_absolute():
        archive = ROOT / archive
    result = {'name': libc, 'link_input': library}
    if archive.is_file():
        result['archive_sha256'] = file_digest(archive)
    manifest = ROOT / 'out/microkernel' / ('newlib' if libc == 'newlib' else 'musl') / platform / 'build/nos-port.json'
    if manifest.is_file():
        result['port_build'] = json.loads(manifest.read_text())
    return result


def git_identity():
    def git(*args):
        return subprocess.check_output(['git', *args], cwd=ROOT)
    untracked = git('ls-files', '--others', '--exclude-standard', '-z').decode().split('\0')
    return {'commit': git('rev-parse', 'HEAD').decode().strip(),
            'status': git('status', '--porcelain').decode().splitlines(),
            'tracked_diff_sha256': hashlib.sha256(git('diff', '--binary', 'HEAD')).hexdigest(),
            'untracked_sha256': {name: file_digest(ROOT / name) for name in untracked
                                  if name and (ROOT / name).is_file()}}


def tool_version(command):
    try:
        return subprocess.check_output([command, '--version'], stderr=subprocess.STDOUT,
                                       text=True, timeout=10).splitlines()[0]
    except (OSError, subprocess.SubprocessError):
        return 'unavailable'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--platform', action='append', choices=PLATFORMS)
    parser.add_argument('--libc', choices=('minimal', 'newlib', 'musl', 'all'), default='all')
    parser.add_argument('--qemu-bindir', default=os.environ.get('QEMU_BINDIR', ''))
    parser.add_argument('--timeout', type=int, default=120)
    parser.add_argument('--cpus', type=int, choices=range(1, 5), default=1)
    parser.add_argument('--disk', type=Path)
    parser.add_argument('--mpu', action='store_true', help='Enable Cortex-M MPU isolation')
    parser.add_argument('--extra-apps', action='store_true', help='Embed and execute real Lua and LVGL applications; requires musl')
    parser.add_argument('--clock-epoch', type=lambda value: int(value, 0), help='Isolated clock boundary profile: initialize guest ticks to this unsigned 64-bit value')
    parser.add_argument('--output-root', type=Path, help='Isolated build directory root')
    parser.add_argument('--clean', action='store_true', help='Rebuild all selected guest images from scratch')
    args = parser.parse_args()
    if args.clock_epoch is not None and not 0 <= args.clock_epoch <= (1 << 64) - 100000:
        parser.error('--clock-epoch must leave room for ticks within uint64')
    results = []
    run_id = time.time_ns()
    report_dir = ROOT / 'out/microkernel/reports' / str(run_id)
    report_dir.mkdir(parents=True)
    platforms = args.platform or list(BASELINE_PLATFORMS)
    if args.cpus != 1 and 'stm32' in platforms:
        parser.error('--cpus > 1 requires an MMU platform selection')
    if args.disk and 'stm32' in platforms:
        parser.error('--disk requires an MMU platform selection')
    complete = (set(platforms) == set(BASELINE_PLATFORMS) and args.libc == 'all'
                and args.cpus == 1 and args.disk is None and not args.mpu and not args.extra_apps and args.clock_epoch is None)
    metadata = {'schema_version': 2, 'run_id': run_id, 'matrix_complete': complete,
                'source': git_identity(), 'configuration': vars(args).copy(),
                'started_ns': time.time_ns(), 'qemu_versions': {}}
    metadata['configuration'] = {key: str(value) if isinstance(value, Path) else value
                                 for key, value in metadata['configuration'].items()}
    atomic_write_changed(report_dir / 'metadata.json', json.dumps(metadata, indent=2) + '\n')
    for platform in platforms:
        libraries = PLATFORM_LIBRARIES[platform] if args.libc == 'all' else [args.libc]
        for libc in libraries:
            out = ((args.output_root or ROOT / 'out/microkernel') / platform / libc).resolve()
            out.mkdir(parents=True, exist_ok=True)
            saved = report_dir / platform / libc
            saved.mkdir(parents=True)
            if args.clean:
                subprocess.run(['make', f'PLATFORM={platform}', f'LIBC={libc}', f'O={out}', 'clean'], cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
                out.mkdir(parents=True, exist_ok=True)
            with (out / 'build.log').open('w') as log:
                clock_flags = [] if args.clock_epoch is None else [f'EXTRA_CFLAGS=-DNOS_CLOCK_TEST_EPOCH={args.clock_epoch}ULL']
                build = subprocess.run(['make', '-j5', f'PLATFORM={platform}', f'LIBC={libc}', f'O={out}', 'BUILD_MODE=selftest', f'SMP={args.cpus}', f'MPU={int(args.mpu)}', f'EXTRA_APPS={int(args.extra_apps)}', *clock_flags], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
            shutil.copy2(out / 'build.log', saved / 'build.log')
            if build.returncode:
                print((out / 'build.log').read_text()[-7000:])
                results.append(dict(platform=platform, libc=libc, passed=False, reason='build', run_id=run_id, matrix_complete=complete))
                continue
            command = qemu_command(platform, out / 'nos.elf', args.qemu_bindir, args.cpus, args.disk)
            metadata['qemu_versions'][command[0]] = tool_version(command[0])
            shutil.copy2(out / 'build-config.json', saved / 'build-config.json')
            configuration = json.loads((out / 'build-config.json').read_text())
            try:
                text, exit_code, runtime_metrics = run_guest(command, saved / 'qemu.log', args.timeout)
            except OSError as exc:
                text = str(exc)
                exit_code = None
                runtime_metrics = {'error': str(exc), 'host_clock_seconds': None}
            (out / 'qemu.log').write_text(text)
            passed = {line[5:] for line in text.splitlines() if line.startswith('PASS ')}
            required = COMMON | (MMU if platform not in ('stm32', 'riscv32_nommu') else {'nommu_explicit_contract', 'nommu_user_privilege'}) | (NEWLIB if libc == 'newlib' else MUSL if libc == 'musl' else set())
            if platform in ('stm32', 'riscv32_nommu'):
                required |= {'elf_pie_relocations_validated'}
            if platform in ('arm_virt', 'arm64_virt', 'riscv_virt', 'riscv32_virt', 'riscv32_nommu'):
                required |= {'fpu_context_switch'}
            if platform.startswith('riscv'):
                required |= {'riscv_halfword_thread_entry'}
            if platform in ('arm64_virt', 'riscv_virt'):
                required |= {'native_width_handle_validation', 'capability_high_bits_validation'}
            if args.cpus > 1:
                required |= {'smp_parallel_shared_threads', 'smp_tlb_shootdown', 'smp_clock_rate'}
            if args.mpu:
                required |= {'mpu_kernel_isolation', 'mpu_readonly_mapping', 'mpu_execute_never'}
            if libc in ('musl', 'newlib'):
                required |= {libc + '_float_format', libc + ('_thread_reent' if platform == 'stm32' else '_process_tls')}
                required |= {'libc_monotonic_sleep', 'libc_monotonic_absolute_sleep'}
                if platform == 'stm32':
                    required |= {'libc_realtime_unsupported'}
                else:
                    required |= {'libc_realtime_clock_gettimeofday', 'libc_realtime_absolute_sleep'}
                required |= {'posix_openat_positioned_io_stat', 'posix_cwd_relative_paths',
                             'posix_directory_iteration_seek', 'posix_directory_long_record',
                             'posix_poll_select_readiness', 'posix_poll_timeout_eof_invalid',
                             'posix_metadata_modes_truncate', 'posix_metadata_links_symlinks'}
                if args.cpus > 1:
                    required |= {libc + '_pthread_cross_cpu_execution'}
            if libc == 'minimal':
                required |= {'shared_thread_create', 'shared_thread_lock', 'shared_thread_join_futex_condition'}
            if libc == 'newlib':
                required |= {'newlib_shared_allocator_errno'}
            if libc != 'minimal' and platform != 'stm32':
                required |= {'elf_libc_tls_app'}
            if args.extra_apps:
                required |= {'lua_user_application', 'lvgl_user_rendering', 'elf_lua_app', 'elf_lvgl_app'}
            if args.clock_epoch is not None:
                required = {'page_allocator', 'mapping_transaction_rollback', 'clock64_kernel_snapshot_boundary',
                            'clock64_legacy_boundary', 'clock64_sleep_progress', 'timer_preemption'}
                if libc != 'minimal':
                    required |= {'libc_clock_epoch_boundary'}
            missing = sorted(required - passed)
            clock_ok = args.cpus == 1 or (runtime_metrics.get('host_clock_seconds') is not None and
                                         runtime_metrics['host_clock_seconds'] >= 0.4)
            good = exit_code == 0 and 'NOS TEST PASS' in text and 'FAIL' not in text and not missing and clock_ok
            results.append(dict(platform=platform, libc=libc, passed=good, exit_code=exit_code, checks=sorted(passed), missing=missing, run_id=run_id, matrix_complete=complete, cpus=args.cpus,
                                image_sha256=file_digest(out / 'nos.elf'), command=command,
                                build_configuration=configuration, libc_identity=libc_identity(platform, libc, configuration),
                                runtime_metrics=runtime_metrics, clock_rate_verified=clock_ok if args.cpus > 1 else None,
                                qemu_version=metadata['qemu_versions'][command[0]]))
            print(f'{platform}/{libc}: {"PASS" if good else "FAIL"} ({len(passed)} guest checks, exit={exit_code})')
            if not good:
                print(text[-7000:])
                print('Missing:', missing)
    metadata['finished_ns'] = time.time_ns()
    metadata['source_after'] = git_identity()
    metadata['source_changed_during_run'] = metadata['source'] != metadata['source_after']
    atomic_write_changed(report_dir / 'metadata.json', json.dumps(metadata, indent=2) + '\n')
    payload = json.dumps(results, indent=2) + '\n'
    atomic_write_changed(report_dir / 'results.json', payload)
    target = ROOT / 'out/microkernel' / ('results.json' if complete else 'results-partial.json')
    atomic_write_changed(target, payload)
    print(f'Results: {report_dir / "results.json"} (complete matrix: {complete})')
    return 0 if results and all(r['passed'] for r in results) else 1

if __name__ == '__main__':
    sys.exit(main())
