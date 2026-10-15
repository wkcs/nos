#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Verified dynamic-reentrant Newlib port."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
from build_signature import compiler_identity

ROOT = Path(__file__).resolve().parents[1]
VERSION = '4.5.0.20241231'
SHA256 = '33f12605e0054965996c25c1382b3e463b0af91799001f5bb8c0630f2ec8c852'
DEFAULT_ARCHIVE = Path('/workspace/.nos-env/newlib-download') / f'newlib_{VERSION}.orig.tar.gz'
CPU_FLAGS = {'arm_virt': '-mcpu=cortex-a15 -marm -mfloat-abi=soft',
             'stm32': '-mcpu=cortex-m4 -mthumb -mfloat-abi=soft'}

def authenticated_download(destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    apt = Path('/workspace/.nos-env/apt')
    sources = apt / 'microkernel-sources.list'
    if not sources.is_file():
        raise SystemExit('Run scripts/setup_microkernel_env.sh to initialize authenticated Debian sources')
    # APT verifies the signed Release/Sources chain and its archive checksum.
    subprocess.run(['/usr/bin/apt-get', '-o', f'Dir::Etc::sourcelist={sources}',
        '-o', 'Dir::Etc::sourceparts=-', '-o', f'Dir::State::lists={apt / "lists"}',
        '-o', f'Dir::Cache::archives={apt / "archives"}', '-o',
        'Dir::Log=/workspace/.nos-env/logs', '-o', 'Debug::NoLocking=1',
        '--download-only', 'source', f'newlib={VERSION}-1'], cwd=destination.parent, check=True)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--platform', choices=CPU_FLAGS, required=True)
    parser.add_argument('--archive', type=Path, default=DEFAULT_ARCHIVE)
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1 or args.jobs > 32:
        parser.error('--jobs must be 1..32')
    if not args.archive.is_file():
        authenticated_download(args.archive)
    if not args.archive.is_file() or hashlib.sha256(args.archive.read_bytes()).hexdigest() != SHA256:
        raise SystemExit('Newlib source checksum mismatch')
    output = ROOT / 'out/microkernel/newlib' / args.platform
    source = output / f'newlib-{VERSION}'
    build, prefix = output / 'build', output / 'install'
    output.mkdir(parents=True, exist_ok=True)
    if not source.is_dir():
        with tarfile.open(args.archive) as archive:
            archive.extractall(output, filter='data')
    flags = CPU_FLAGS[args.platform] + ' -O2 -g -D__DYNAMIC_REENT__ -ffunction-sections -fdata-sections'
    configure = ['--target=arm-none-eabi', f'--prefix={prefix}', '--disable-multilib',
        '--disable-libgloss', '--disable-newlib-supplied-syscalls', '--enable-newlib-multithread',
        '--enable-newlib-retargetable-locking', '--enable-newlib-io-c99-formats',
        '--enable-newlib-io-long-long', '--enable-newlib-io-float', '--disable-nls', '--disable-werror']
    identity = {'archive': SHA256, 'compiler': compiler_identity('arm-none-eabi-gcc'),
        'cflags': flags, 'configure': configure,
        'script': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    stamp = build / 'nos-port.json'
    if stamp.is_file() and json.loads(stamp.read_text()) != identity:
        shutil.rmtree(build)
        if prefix.exists(): shutil.rmtree(prefix)
    build.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, CFLAGS_FOR_TARGET=flags, CC_FOR_TARGET='arm-none-eabi-gcc',
        AR_FOR_TARGET='arm-none-eabi-ar', RANLIB_FOR_TARGET='arm-none-eabi-ranlib', MAKEINFO='true')
    if not stamp.is_file():
        subprocess.run([str(source / 'configure'), *configure], cwd=build, env=environment, check=True)
    subprocess.run(['make', f'-j{args.jobs}', 'all-target-newlib', 'MAKEINFO=true'],
        cwd=build, env=environment, check=True)
    subprocess.run(['make', 'install-target-newlib', 'MAKEINFO=true'], cwd=build, env=environment, check=True)
    for name in ('include', 'lib'):
        link = prefix / name
        target = Path('arm-none-eabi') / name
        if not link.exists(): link.symlink_to(target, target_is_directory=True)
    stamp.write_text(json.dumps(identity, sort_keys=True) + '\n')
    print(prefix)

if __name__ == '__main__':
    main()
