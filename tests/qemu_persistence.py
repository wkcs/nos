#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Verify file storage across two real boots."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from qemu_platforms import qemu_command


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--platform', choices=('arm_virt', 'arm64_virt', 'riscv_virt', 'riscv32_virt', 'riscv32_nommu'), default='arm64_virt')
    parser.add_argument('--libc', choices=('minimal', 'musl', 'newlib'), default='minimal')
    parser.add_argument('--qemu-bindir', default=os.environ.get('QEMU_BINDIR', ''))
    parser.add_argument('--timeout', type=int, default=120)
    parser.add_argument('--filesystem', choices=('native', 'native-rollback', 'fat'), default='native')
    parser.add_argument('--virtio-version', choices=('default', 'modern', 'legacy'), default='default')
    args = parser.parse_args()
    run_id = time.time_ns()
    out = ROOT / 'out/microkernel/persistence' / str(run_id)
    out.mkdir(parents=True)
    disk = out / 'disk.raw'
    with disk.open('wb') as stream:
        stream.truncate(4 * 1024 * 1024)
    results = []
    phases = {'native': [(1, 'vfs_persistent_write'), (2, 'vfs_persistent_read')],
              'native-rollback': [(5, 'vfs_persistent_two_generations'), (2, 'vfs_persistent_read')],
              'fat': [(3, 'vfs_fat_write'), (4, 'vfs_fat_read')]}[args.filesystem]
    for phase, marker in phases:
        build_dir = out / f'phase{phase}'
        build_dir.mkdir()
        with (build_dir / 'build.log').open('w') as log:
            build = subprocess.run(['make', '-j5', f'PLATFORM={args.platform}', f'LIBC={args.libc}',
                                    f'O={build_dir}', 'BUILD_MODE=selftest', f'PERSISTENCE_TEST={phase}'],
                                   cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        if build.returncode:
            print((build_dir / 'build.log').read_text()[-7000:])
            results.append({'phase': phase, 'passed': False, 'reason': 'build'})
            break
        extra_args = [] if args.virtio_version == 'default' else ['-global',
            'virtio-mmio.force-legacy=' + ('false' if args.virtio_version == 'modern' else 'true')]
        command = qemu_command(args.platform, build_dir / 'nos.elf', args.qemu_bindir, disk=disk, extra_args=extra_args)
        exit_code = None
        with (build_dir / 'qemu.log').open('wb') as log:
            try:
                run = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout)
                exit_code = run.returncode
            except subprocess.TimeoutExpired:
                pass
        text = (build_dir / 'qemu.log').read_text(errors='replace')
        required = [marker, 'virtio_hardware_irq', 'virtio_transport_version']
        good = exit_code == 0 and all(f'PASS {check}\n' in text for check in required) and 'NOS TEST PASS' in text and 'FAIL' not in text
        results.append({'phase': phase, 'passed': good, 'exit_code': exit_code,
                        'required': required, 'missing': [check for check in required if f'PASS {check}\n' not in text],
                        'image_sha256': hashlib.sha256((build_dir / 'nos.elf').read_bytes()).hexdigest(),
                        'disk_sha256': hashlib.sha256(disk.read_bytes()).hexdigest(), 'command': command})
        print(f'{args.platform}: persistent phase {phase}: {"PASS" if good else "FAIL"}')
        if not good:
            print(text[-7000:])
            break
        if args.filesystem == 'native-rollback' and phase == 5:
            # Damage a byte in the latest bank after a clean shutdown.  The
            # second boot must verify its CRC and recover the preceding bank.
            with disk.open('r+b') as stream:
                stream.seek(512)
                original = stream.read(1)
                stream.seek(512)
                stream.write(bytes([original[0] ^ 1]))
            results[-1]['fault_injection'] = {'offset': 512, 'xor': 1,
                'corrupted_disk_sha256': hashlib.sha256(disk.read_bytes()).hexdigest()}
    report = {'run_id': run_id, 'platform': args.platform, 'libc': args.libc, 'filesystem': args.filesystem,
              'virtio_version': args.virtio_version,
              'passed': len(results) == 2 and all(result['passed'] for result in results), 'boots': results}
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(out / 'results.json')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
