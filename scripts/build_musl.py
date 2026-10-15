#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Build a static musl port from verified source."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import re
import subprocess
import tarfile
from build_signature import compiler_identity

SHA256 = 'a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4'
ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--archive', required=True, type=Path, help='Authenticated musl 1.2.5 source tarball')
    parser.add_argument('--platform', required=True, choices=('arm64_virt', 'riscv_virt'))
    args = parser.parse_args()
    if hashlib.sha256(args.archive.read_bytes()).hexdigest() != SHA256:
        raise SystemExit('musl source checksum mismatch')
    out = ROOT / 'out/microkernel/musl' / args.platform
    source = out / 'musl-1.2.5'
    out.mkdir(parents=True, exist_ok=True)
    if not source.exists():
        with tarfile.open(args.archive) as archive:
            archive.extractall(out, filter='data')
    arch = 'aarch64' if args.platform == 'arm64_virt' else 'riscv64'
    header = ['/* NOS OS port: generated; upstream source archive is unchanged. */',
              '#define __SYSCALL_LL_E(x) (x)', '#define __SYSCALL_LL_O(x) (x)',
              'long nos_musl_syscall(long, long, long, long, long, long, long);']
    for n in range(7):
        params = ['long n'] + [f'long a{i}' for i in range(n)]
        values = ['n'] + [f'a{i}' for i in range(n)] + ['0'] * (6 - n)
        header.append(f'static inline long __syscall{n}({", ".join(params)}) {{ return nos_musl_syscall({", ".join(values)}); }}')
    header += ['#define IPC_64 0']
    port_header = source / 'arch' / arch / 'syscall_arch.h'
    contents = '\n'.join(header) + '\n'
    if port_header.read_text() != contents:
        port_header.write_text(contents)
    pthread_header = source / 'src/internal/pthread_impl.h'
    pthread_contents = pthread_header.read_text()
    for name, value in [('DEFAULT_STACK_SIZE', 16384), ('DEFAULT_GUARD_SIZE', 4096)]:
        pthread_contents = re.sub(rf'(#define {name})\s+\d+', rf'\g<1> {value}', pthread_contents)
    if pthread_header.read_text() != pthread_contents:
        pthread_header.write_text(pthread_contents)
    build, prefix = out / 'build', out / 'install'
    build.mkdir(exist_ok=True)
    cc = 'aarch64-linux-gnu-gcc -mno-outline-atomics' if arch == 'aarch64' else 'riscv64-linux-gnu-gcc -march=rv64gc -mabi=lp64d -mno-relax -msmall-data-limit=0'
    cross = arch + '-linux-gnu-'
    stamp = build / 'nos-port.json'
    identity = {'archive': SHA256, 'header': hashlib.sha256(contents.encode()).hexdigest(),
                'pthread_config': hashlib.sha256(pthread_contents.encode()).hexdigest(),
                'port_script': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                'compiler': compiler_identity(cc.split()[0]),
                'cc': cc, 'prefix': str(prefix), 'malloc': 'oldmalloc',
                'configure': ['--disable-shared', '--disable-wrapper', '--with-malloc=oldmalloc']}
    if not stamp.exists() or json.loads(stamp.read_text()) != identity:
        # Configure flags and compiler changes must invalidate previously compiled libc objects.
        if (build / 'Makefile').exists():
            subprocess.run(['make', 'clean'], cwd=build, check=True)
        subprocess.run([str(source / 'configure'), f'--target={arch}-linux-gnu', '--disable-shared', '--disable-wrapper', '--with-malloc=oldmalloc', f'--prefix={prefix}', f'CC={cc}', f'AR={cross}ar', f'RANLIB={cross}ranlib'], cwd=build, check=True)
        stamp.write_text(json.dumps(identity))
    subprocess.run(['make', '-j5', 'lib/libc.a', 'install-headers'], cwd=build, check=True)
    (prefix / 'lib').mkdir(exist_ok=True)
    shutil.copy2(build / 'lib/libc.a', prefix / 'lib/libc.a')
    print(prefix)

if __name__ == '__main__':
    main()
