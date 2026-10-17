#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Build unmodified pinned OpenPOSIX tests."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from build_signature import atomic_write_changed

ROOT = Path(__file__).resolve().parents[1]
COMMIT = 'd2550ffbbcfe163212cd7e9c132db65ae0fa06ed'
TAG = '20250930'
PREFIX = 'testcases/open_posix_testsuite'
PLATFORMS = {
    'arm_virt': ['-mcpu=cortex-a15', '-marm', '-mfloat-abi=soft'],
    'arm64_virt': ['-mcpu=cortex-a53', '-mno-outline-atomics'],
    'riscv_virt': ['-march=rv64gc', '-mabi=lp64d', '-mcmodel=medany', '-mno-relax', '-msmall-data-limit=0'],
}
DEFAULT_CASES = [name + '/1-1' for name in (
    'pthread_create', 'pthread_join', 'pthread_mutex_init', 'pthread_mutex_lock',
    'pthread_mutex_unlock', 'pthread_once', 'pthread_key_create', 'pthread_setspecific',
    'pthread_getspecific', 'pthread_cond_init', 'pthread_cond_signal', 'pthread_cond_broadcast',
    'pthread_cond_wait', 'sigaction', 'kill', 'sigpending', 'sigsuspend', 'clock_gettime',
    'clock_getres', 'clock_nanosleep', 'nanosleep', 'sem_init', 'sem_post', 'sem_wait',
    'fork', 'mmap', 'munmap')]
EXTRA_CASES = [name + '/1-1' for name in (
    'sem_open', 'sem_close', 'sem_getvalue', 'sem_timedwait', 'sem_unlink',
    'sigwait', 'sigtimedwait', 'sigwaitinfo', 'sigaltstack',
    'pthread_rwlock_init', 'pthread_rwlock_rdlock', 'pthread_rwlock_wrlock', 'pthread_rwlock_unlock',
    'pthread_barrier_init', 'pthread_barrier_wait', 'pthread_cond_timedwait', 'pthread_mutex_timedlock',
    'pthread_spin_init', 'pthread_spin_lock', 'pthread_spin_unlock', 'shm_open', 'shm_unlink')]
EXTRA_CASES += ['sigprocmask/4-1', 'sem_wait/12-1']
CASE_SELECTIONS = {'baseline': DEFAULT_CASES, 'expanded': DEFAULT_CASES + EXTRA_CASES}


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', required=True, choices=PLATFORMS)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--source', type=Path, default=Path('/workspace/.nos-env/src/ltp-' + TAG))
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument('--case', action='append', help='Unmodified interface/case, e.g. pthread_create/1-1')
    selection.add_argument('--selection', choices=CASE_SELECTIONS, default='baseline')
    parser.add_argument('--manifest', type=Path, default=Path('/workspace/.nos-env/glibc/manifest.json'))
    args = parser.parse_args()
    source, out = args.source.resolve(), args.output.resolve()
    if not source.exists():
        source.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['git', 'clone', '--depth=1', '--branch', TAG,
                        'https://github.com/linux-test-project/ltp.git', str(source)], check=True)
    actual = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != COMMIT:
        raise SystemExit('OpenPOSIX upstream commit mismatch: ' + actual)
    dirty = subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain', '--', PREFIX], text=True)
    if dirty:
        raise SystemExit('OpenPOSIX source modifications are forbidden: ' + dirty)
    selected = args.case or CASE_SELECTIONS[args.selection]
    if len(set(selected)) != len(selected):
        raise SystemExit('Duplicate selected case')
    suite = source / PREFIX
    manifest = json.loads(args.manifest.read_text())
    identity = manifest['platforms'][args.platform]
    compiler = identity['compiler']['command']
    for artifact in [identity['compiler'], *identity['libraries'].values()]:
        path = artifact.get('file', artifact.get('path'))
        if digest(path) != artifact['sha256']:
            raise SystemExit('Toolchain/library SHA256 mismatch: ' + path)
    out.mkdir(parents=True, exist_ok=True)
    cases = []
    for name in selected:
        relative = Path(name + '.c')
        if relative.is_absolute() or '..' in relative.parts or len(relative.parts) != 2:
            raise SystemExit('Invalid interface/case: ' + name)
        test = suite / 'conformance/interfaces' / relative
        if not test.is_file():
            raise SystemExit('Unknown upstream test: ' + name)
        stem = name.replace('/', '-')
        elf, deps, log = out / (stem + '.elf'), out / (stem + '.d'), out / (stem + '.build.log')
        command = [compiler, *PLATFORMS[args.platform], '--sysroot=' + identity['sysroot'],
                   '-isystem', identity['include'], '-I' + str(suite / 'include'),
                   '-std=c99', '-D_POSIX_C_SOURCE=200809L', '-D_XOPEN_SOURCE=700',
                   '-Wall', '-Wextra', '-O2', '-g', '-fPIE', '-pie',
                   '-Wl,--build-id=none,--dynamic-linker=' + identity['interpreter'],
                   '-MD', '-MF', str(deps), str(test), '-pthread', '-lrt', '-lm', '-ldl', '-o', str(elf)]
        with log.open('w') as stream:
            run = subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT)
        item = {'name': name, 'source': str(test), 'source_sha256': digest(test),
                'archive_path': '/bin/posix/' + stem, 'command': command,
                'compile_returncode': run.returncode,
                'classification': 'compiled' if not run.returncode else 'compile_failed', 'build_log': str(log)}
        if not run.returncode:
            dependencies = deps.read_text().replace('\\\n', ' ').split(':', 1)[1].split()
            item.update(elf=str(elf), elf_sha256=digest(elf),
                        dependencies_sha256={path: digest(path) for path in sorted(set(dependencies))})
        cases.append(item)
        print(args.platform + '/' + name + ': ' + item['classification'], flush=True)
    compiled = [item for item in cases if item['classification'] == 'compiled']
    header = ['/* Generated from unmodified OpenPOSIX upstream tests. */', '#ifndef NOS_POSIX_CASES_H',
              '#define NOS_POSIX_CASES_H', 'struct nos_posix_case { const char *name, *path; };',
              'static const struct nos_posix_case nos_posix_cases[] = {']
    header.extend('    {' + json.dumps(item['name']) + ', ' + json.dumps(item['archive_path']) + '},' for item in compiled)
    header += ['};', '#define NOS_POSIX_CASE_COUNT ' + str(len(compiled)) + 'u', '#endif', '']
    atomic_write_changed(out / 'posix_cases.h', '\n'.join(header))
    make = ['# Generated OpenPOSIX archive inputs.',
            'POSIX_SUITE_IMAGES := ' + ' '.join(item['elf'] for item in compiled),
            'POSIX_SUITE_FILES := ' + ' '.join('--file ' + item['archive_path'] + '=' + item['elf'] for item in compiled), '']
    atomic_write_changed(out / 'posix-files.mk', '\n'.join(make))
    result = {'upstream': 'https://github.com/linux-test-project/ltp', 'tag': TAG, 'commit': actual,
              'source_tree_clean': True, 'upstream_assertions_modified': False,
              'upstream_build_policy': {'file': str(suite / 'include/mk/config.mk.in'),
                                        'sha256': digest(suite / 'include/mk/config.mk.in'),
                                        'standard_flags': '-std=c99 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Wall -Wextra'},
              'platform': args.platform, 'glibc': identity, 'cases': cases,
              'selection': 'custom' if args.case else args.selection,
              'full_posix_conformance': False}
    dirty_after = subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain', '--', PREFIX], text=True)
    if dirty_after:
        raise SystemExit('OpenPOSIX source changed during compilation: ' + dirty_after)
    atomic_write_changed(out / 'manifest.json', json.dumps(result, indent=2) + '\n')
    return 0 if len(compiled) == len(cases) else 1


if __name__ == '__main__':
    raise SystemExit(main())
