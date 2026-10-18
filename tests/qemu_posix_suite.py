#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Selected, unmodified OpenPOSIX guest tests."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

from qemu_glibc import PLATFORMS, RNG, LINUX_KERNEL, LINUX_GC_KERNEL, LINUX_SINGLE_CPU_KERNEL
from qemu_matrix import ROOT, COMMON, MMU, file_digest, git_identity, tool_version
from qemu_guest import run_guest
from qemu_platforms import qemu_command

STATUS = {0: 'passed', 1: 'failed', 2: 'unresolved', 4: 'unsupported', 5: 'untested'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', action='append', choices=PLATFORMS)
    parser.add_argument('--source', type=Path, default=Path('/workspace/.nos-env/src/ltp-20250930'))
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument('--case', action='append', help='Upstream interface/case (repeatable)')
    selection.add_argument('--selection', choices=('baseline', 'expanded'), default='baseline',
                           help='27 baseline or 51 expanded unmodified upstream cases')
    parser.add_argument('--case-ticks', type=int, default=3000, help='Real guest deadline at 100 Hz, default 30 seconds')
    parser.add_argument('--timeout', type=int, help='Emergency host deadline; expiry always fails')
    parser.add_argument('--cpus', type=int, choices=(1, 2, 4), default=1)
    parser.add_argument('--output-root', type=Path)
    parser.add_argument('--qemu-bindir', default=os.environ.get('QEMU_BINDIR', ''))
    args = parser.parse_args()
    if not 1 <= args.case_ticks <= 60000:
        parser.error('--case-ticks must be in 1..60000')
    run_id = time.time_ns()
    report = ROOT / 'out/microkernel/posix-suite-reports' / str(run_id)
    report.mkdir(parents=True)
    metadata = {'run_id': run_id, 'source': git_identity(), 'full_posix_conformance': False,
                'started_ns': time.time_ns(), 'completed': False,
                'upstream_assertions_modified': False, 'case_deadline_ticks': args.case_ticks,
                'selection': 'custom' if args.case else args.selection,
                'status_codes': STATUS, 'nonzero_counts_as_pass': False}
    (report / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
    results = []
    for platform in args.platform or PLATFORMS:
        saved = report / platform
        saved.mkdir()
        out = ((args.output_root or report / 'build') / platform).resolve()
        out.mkdir(parents=True, exist_ok=True)
        # A prerequisite failure must never reuse a manifest from an older selection.
        (out / 'manifest.json').unlink(missing_ok=True)
        command = ['python3', 'scripts/build_posix_suite.py', '--platform', platform,
                   '--source', str(args.source), '--output', str(out)]
        for case in args.case or []:
            command += ['--case', case]
        if not args.case:
            command += ['--selection', args.selection]
        with (saved / 'case-build.log').open('w') as stream:
            compiled = subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT)
        item = {'platform': platform, 'passed': False, 'case_builder_returncode': compiled.returncode,
                'cpus': args.cpus, 'cases': []}
        results.append(item)
        if not (out / 'manifest.json').is_file():
            item['reason'] = 'case_build_prerequisite'
            print((saved / 'case-build.log').read_text()[-3000:])
            continue
        manifest = json.loads((out / 'manifest.json').read_text())
        shutil.copy2(out / 'manifest.json', saved / 'manifest.json')
        (saved / 'case-build').mkdir()
        for case in manifest['cases']:
            shutil.copy2(case['build_log'], saved / 'case-build' / (case['name'].replace('/', '-') + '.log'))
        item['upstream_commit'] = manifest['commit']
        item['cases'] = [{'name': case['name'], 'classification': case['classification'],
                          'compile_returncode': case['compile_returncode']} for case in manifest['cases']]
        available = {case['name'] for case in manifest['cases'] if case['classification'] == 'compiled'}
        if not available:
            item['reason'] = 'no_compiled_cases'
            continue
        build_command = ['make', '-j5', f'PLATFORM={platform}', 'LIBC=minimal', 'LINUX_COMPAT=1',
                         'POSIX_SUITE=1', f'POSIX_CASE_TICKS={args.case_ticks}', 'BUILD_MODE=selftest',
                         f'SMP={args.cpus}', f'O={out}']
        item['build_command'] = build_command
        with (saved / 'kernel-build.log').open('w') as stream:
            build = subprocess.run(build_command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT)
        item['build_returncode'] = build.returncode
        if build.returncode:
            item['reason'] = 'kernel_build'
            print((saved / 'kernel-build.log').read_text()[-4000:])
            continue
        shutil.copy2(out / 'build-config.json', saved / 'build-config.json')
        command = qemu_command(platform, out / 'nos.elf', args.qemu_bindir, args.cpus, extra_args=RNG)
        item.update(command=command, image_sha256=file_digest(out / 'nos.elf'), qemu_version=tool_version(command[0]))
        timeout = args.timeout or int(len(available) * args.case_ticks / 100 + 120)
        text, code, metrics = run_guest(command, saved / 'qemu.log', timeout)
        observed, errors, cleanup = {}, [], {}
        for line in text.splitlines():
            match = re.fullmatch(r'NOS POSIX RESULT (\S+) (-?\d+)', line)
            expiry = re.fullmatch(r'NOS POSIX TIMEOUT (\S+)', line)
            failed_cleanup = re.fullmatch(r'NOS POSIX CLEANUP_ERROR (\S+) (-?\d+)', line)
            if failed_cleanup:
                if failed_cleanup[1] not in available or failed_cleanup[1] in cleanup:
                    errors.append(line)
                cleanup[failed_cleanup[1]] = int(failed_cleanup[2])
            elif match or expiry:
                name = match[1] if match else expiry[1]
                if name not in available or name in observed:
                    errors.append(line)
                else:
                    observed[name] = int(match[2]) if match else 'timeout'
            elif line.startswith('NOS POSIX '):
                errors.append(line)
        for case in item['cases']:
            name = case['name']
            if name not in available:
                continue
            status = observed.get(name)
            case['guest_status'] = status
            case['classification'] = ('missing_result' if status is None else 'timeout' if status == 'timeout'
                                      else STATUS.get(status, 'guest_error'))
            if name in cleanup:
                case.update(classification='cleanup_failed', cleanup_status=cleanup[name])
        checks = {line[5:] for line in text.splitlines() if line.startswith('PASS ')}
        required = COMMON | MMU | LINUX_KERNEL | LINUX_GC_KERNEL | {'fpu_context_switch', 'shared_thread_create', 'shared_thread_lock',
                                   'shared_thread_join_futex_condition', 'open_posix_selected_cases'}
        if args.cpus == 1:
            required |= LINUX_SINGLE_CPU_KERNEL
        if platform.startswith('riscv'):
            required |= {'riscv_halfword_thread_entry'}
        if platform == 'arm_virt':
            required |= {'arm_linux_kuser_helpers'}
        if platform != 'arm_virt':
            required |= {'native_width_handle_validation', 'capability_high_bits_validation'}
        if args.cpus > 1:
            required |= {'smp_parallel_shared_threads', 'smp_tlb_shootdown', 'smp_clock_rate'}
        missing = sorted(required - checks)
        clock_ok = args.cpus == 1 or (metrics.get('host_clock_seconds') is not None and metrics['host_clock_seconds'] >= 0.4)
        item.update(exit_code=code, runtime_metrics=metrics, protocol_errors=errors, cleanup_errors=cleanup, missing=missing,
                    checks=sorted(checks), required=sorted(required),
                    passed=code == 0 and 'NOS TEST PASS' in text and not missing and not errors and not cleanup and clock_ok
                    and all(case['classification'] == 'passed' for case in item['cases']))
        print(platform + ': ' + ('PASS' if item['passed'] else 'FAIL') + ' ' +
              json.dumps({kind: sum(case['classification'] == kind for case in item['cases'])
                          for kind in sorted({case['classification'] for case in item['cases']})}), flush=True)
        for case in item['cases']:
            if case['classification'] != 'passed':
                print('  ' + case['name'] + ': ' + case['classification'], flush=True)
    metadata['source_after'] = git_identity()
    metadata['source_changed_during_run'] = metadata['source'] != metadata['source_after']
    metadata.update(completed=True, finished_ns=time.time_ns())
    (report / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
    (report / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print(report / 'results.json', flush=True)
    return 0 if results and all(item['passed'] for item in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
