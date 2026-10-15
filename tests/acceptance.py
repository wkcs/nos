#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Reproducible phase 0-5 software acceptance."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--section', action='append', choices=('baseline', 'build', 'normal', 'storage', 'smp', 'mpu', 'rv32', 'applications', 'faults', 'clock'))
    args = parser.parse_args()
    sections = set(args.section or ('baseline', 'build', 'normal', 'storage', 'smp', 'mpu', 'rv32', 'applications', 'faults', 'clock'))
    run_id = time.time_ns()
    out = ROOT / 'out/microkernel/acceptance' / str(run_id)
    out.mkdir(parents=True)
    commands = []
    def add(section, name, script, *flags):
        if section in sections:
            commands.append((section, name, ['python3', script, *map(str, flags)]))
    add('baseline', 'eight_platform_libc_combinations', 'tests/qemu_matrix.py')
    add('build', 'incremental_configuration', 'tests/build_configuration.py', '--platform', 'arm64_virt', '--libc', 'musl')
    add('build', 'vfs_host_sanitizers', 'tests/host_vfs.py')
    for platform in ('arm_virt', 'arm64_virt', 'riscv_virt'):
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
    results = []
    environment = os.environ.copy()
    environment['PYTHONUNBUFFERED'] = '1'
    for section, name, command in commands:
        start = time.monotonic()
        print(f'RUN {section}/{name}', flush=True)
        with (out / (name + '.log')).open('w') as log:
            try:
                run = subprocess.run(command, cwd=ROOT, env=environment, stdout=log, stderr=subprocess.STDOUT, timeout=900)
                code = run.returncode
            except subprocess.TimeoutExpired:
                code = None
        good = code == 0
        results.append({'section': section, 'name': name, 'passed': good,
                        'returncode': code, 'elapsed_seconds': time.monotonic() - start, 'command': command})
        (out / 'results.json').write_text(json.dumps({'run_id': run_id, 'sections': sorted(sections),
            'passed': all(result['passed'] for result in results), 'completed': len(results),
            'total': len(commands), 'tests': results}, indent=2) + '\n')
        print(f'{"PASS" if good else "FAIL"} {section}/{name}', flush=True)
        if not good:
            print((out / (name + '.log')).read_text()[-4000:], flush=True)
    print(f'Acceptance results: {out / "results.json"}')
    return 0 if results and all(result['passed'] for result in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
