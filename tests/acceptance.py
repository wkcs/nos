#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Reproducible phase 0-5 software acceptance."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time
from qemu_matrix import git_identity

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--section', action='append', choices=('baseline', 'build', 'normal', 'storage', 'smp', 'mpu', 'rv32', 'applications', 'faults', 'clock', 'glibc', 'posix-vfs', 'posix-path', 'posix-suite', 'posix-suite-expanded', 'shared-files'))
    parser.add_argument('--include-posix', action='store_true', help='Add the 18 POSIX/glibc groups to the default 54 groups')
    parser.add_argument('--dry-run', action='store_true', help='Print the exact command plan without running checks or claiming results')
    parser.add_argument('--require-frozen-source', action='store_true', help='Fail final acceptance if repository source identity changes during the run')
    args = parser.parse_args()
    sections = set(args.section or ('baseline', 'build', 'normal', 'storage', 'smp', 'mpu', 'rv32', 'applications', 'faults', 'clock'))
    if args.include_posix:
        sections |= {'shared-files', 'posix-vfs', 'posix-path', 'glibc', 'posix-suite-expanded'}
    run_id = time.time_ns()
    out = ROOT / 'out/microkernel/acceptance' / str(run_id)
    commands = []
    def add(section, name, script, *flags):
        if section in sections:
            commands.append((section, name, ['python3', script, *map(str, flags)]))
    add('baseline', 'eight_platform_libc_combinations', 'tests/qemu_matrix.py')
    add('build', 'incremental_configuration', 'tests/build_configuration.py', '--platform', 'arm64_virt', '--libc', 'musl')
    add('build', 'vfs_host_sanitizers', 'tests/host_vfs.py')
    add('build', 'nshell_host_sanitizers', 'tests/host_shell.py')
    for platform in ('arm_virt', 'arm64_virt', 'riscv_virt'):
        add('shared-files', f'{platform}_shared_file_pages', 'tests/qemu_shared_files.py', '--platform', platform,
            '--output-root', out / 'shared-files')
        add('posix-path', f'{platform}_long_path_bulk', 'tests/qemu_long_paths.py', '--platform', platform,
            '--output-root', out / 'posix-path')
        add('posix-suite', f'{platform}_openposix_selected', 'tests/qemu_posix_suite.py', '--platform', platform,
            '--output-root', out / 'posix-suite')
        add('posix-suite-expanded', f'{platform}_openposix_expanded', 'tests/qemu_posix_suite.py', '--platform', platform,
            '--selection', 'expanded', '--output-root', out / 'posix-suite-expanded')
        add('posix-vfs', f'{platform}_posix_archive_entropy', 'tests/qemu_posix_vfs.py', '--platform', platform,
            '--output-root', out / 'posix-vfs')
        for cpus in (1, 4):
            add('glibc', f'{platform}_glibc_{cpus}_cores', 'tests/qemu_glibc.py', '--platform', platform,
                '--cpus', cpus, '--output-root', out / ('glibc-' + str(cpus)))
        add('normal', f'{platform}_normal_uart', 'tests/qemu_normal.py', '--platform', platform)
        for filesystem in ('native', 'fat'):
            add('storage', f'{platform}_{filesystem}', 'tests/qemu_persistence.py', '--platform', platform, '--filesystem', filesystem)
            if platform != 'arm64_virt':
                add('storage', f'{platform}_modern_{filesystem}', 'tests/qemu_persistence.py', '--platform', platform,
                    '--filesystem', filesystem, '--virtio-version', 'modern')
        add('smp', f'{platform}_two_cores', 'tests/qemu_matrix.py', '--platform', platform, '--libc', 'minimal',
            '--cpus', 2, '--output-root', out / 'smp')
        add('smp', f'{platform}_four_cores', 'tests/qemu_matrix.py', '--platform', platform, '--libc', 'minimal',
            '--cpus', 4, '--output-root', out / 'smp4')
        library = 'newlib' if platform == 'arm_virt' else 'musl'
        for cpus in (2, 4):
            add('smp', f'{platform}_{library}_{cpus}_cores', 'tests/qemu_matrix.py', '--platform', platform,
                '--libc', library, '--cpus', cpus, '--output-root', out / ('smp-libc-' + str(cpus)))
    add('normal', 'stm32_normal_boot', 'tests/qemu_normal.py', '--platform', 'stm32', '--boot-only')
    for platform in ('riscv_virt', 'riscv32_virt', 'riscv32_nommu'):
        add('normal', f'{platform}_uart_pending_rearm', 'tests/qemu_uart_rearm.py',
            '--platform', platform, '--output-root', out / 'uart-rearm')
    for filesystem in ('native', 'fat', 'native-rollback'):
        add('storage', f'arm64_modern_{filesystem}', 'tests/qemu_persistence.py', '--platform', 'arm64_virt',
            '--filesystem', filesystem, '--virtio-version', 'modern')
    for libc in ('minimal', 'newlib'):
        add('mpu', f'stm32_mpu_{libc}', 'tests/qemu_matrix.py', '--platform', 'stm32', '--libc', libc,
            '--mpu', '--output-root', out / 'mpu')
    for platform in ('riscv32_virt', 'riscv32_nommu'):
        add('rv32', platform, 'tests/qemu_matrix.py', '--platform', platform, '--libc', 'minimal', '--output-root', out / 'rv32')
    for platform in ('arm_virt', 'stm32', 'riscv32_virt', 'riscv32_nommu'):
        add('clock', f'{platform}_clock64_boundary', 'tests/qemu_matrix.py', '--platform', platform,
            '--libc', 'newlib' if platform in ('arm_virt', 'stm32') else 'minimal', '--clock-epoch', '0x180000000',
            '--output-root', out / 'clock')
    for platform in ('arm_virt', 'arm64_virt', 'riscv_virt', 'stm32', 'riscv32_virt', 'riscv32_nommu'):
        add('faults', f'{platform}_kernel_origin_fault', 'tests/qemu_kernel_fault.py', '--platform', platform)
    for platform in ('arm64_virt', 'riscv_virt'):
        add('applications', f'{platform}_libraries', 'scripts/build_user_libraries.py', '--platform', platform)
        add('applications', f'{platform}_lua_lvgl', 'tests/qemu_matrix.py', '--platform', platform,
            '--libc', 'musl', '--extra-apps', '--output-root', out / 'applications')
    if args.dry_run:
        print(json.dumps({'executed': False, 'sections': sorted(sections), 'total': len(commands),
                          'commands': [{'section': section, 'name': name, 'command': command}
                                       for section, name, command in commands]}, indent=2))
        return 0
    out.mkdir(parents=True)
    results = []
    source_before = git_identity()
    source_changed = False
    def save_results(source_after=None):
        complete = len(results) == len(commands)
        payload = {'run_id': run_id, 'sections': sorted(sections),
                   'passed': bool(results) and complete and all(result['passed'] for result in results)
                             and not (args.require_frozen_source and source_changed),
                   'complete': complete, 'completed': len(results), 'total': len(commands),
                   'require_frozen_source': args.require_frozen_source, 'source': source_before,
                   'source_after': source_after, 'source_changed_during_run': source_changed,
                   'tests': results}
        (out / 'results.json').write_text(json.dumps(payload, indent=2) + '\n')
        return payload
    save_results()
    environment = os.environ.copy()
    environment['PYTHONUNBUFFERED'] = '1'
    for section, name, command in commands:
        start = time.monotonic()
        print(f'RUN {section}/{name}', flush=True)
        with (out / (name + '.log')).open('w') as log:
            try:
                run = subprocess.run(command, cwd=ROOT, env=environment, stdout=log, stderr=subprocess.STDOUT,
                                     timeout=2400 if section.startswith('posix-suite') else 900)
                code = run.returncode
            except subprocess.TimeoutExpired:
                code = None
        good = code == 0
        results.append({'section': section, 'name': name, 'passed': good,
                        'returncode': code, 'elapsed_seconds': time.monotonic() - start, 'command': command})
        source_after = git_identity()
        source_changed |= source_after != source_before
        save_results(source_after)
        print(f'{"PASS" if good else "FAIL"} {section}/{name}', flush=True)
        if not good:
            print((out / (name + '.log')).read_text()[-4000:], flush=True)
    print(f'Acceptance results: {out / "results.json"}')
    source_after = git_identity()
    source_changed |= source_after != source_before
    final = save_results(source_after)
    if args.require_frozen_source and source_changed:
        print('FAIL source identity changed during acceptance', flush=True)
    return 0 if final['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
