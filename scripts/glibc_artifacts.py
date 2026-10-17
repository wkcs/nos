#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Audit genuine Debian glibc artifacts."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile
from build_signature import atomic_write_changed, compiler_identity

ENV = Path('/workspace/.nos-env')
ROOT = ENV / 'root'
APT_OPTIONS = ['-o', f'Dir::Etc::sourcelist={ENV}/apt/microkernel-sources.list',
               '-o', 'Dir::Etc::sourceparts=-', '-o', f'Dir::State::lists={ENV}/apt/lists']
PLATFORMS = {
    'arm_virt': ('arm-linux-gnueabi', 'armel', 'ld-linux.so.3'),
    'arm64_virt': ('aarch64-linux-gnu', 'arm64', 'ld-linux-aarch64.so.1'),
    'riscv_virt': ('riscv64-linux-gnu', 'riscv64', 'ld-linux-riscv64-lp64d.so.1'),
}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def control_paragraphs(text):
    records = []
    for paragraph in text.split('\n\n'):
        current, previous = {}, None
        for line in paragraph.splitlines():
            if line[:1].isspace() and previous:
                current[previous] += '\n' + line.strip()
            elif ': ' in line:
                previous, value = line.split(': ', 1)
                current[previous] = value
            elif line.endswith(':'):
                previous = line[:-1]; current[previous] = ''
        if current:
            records.append(current)
    return records


def apt_records(*arguments):
    return control_paragraphs(subprocess.check_output(['/usr/bin/apt-cache', *APT_OPTIONS, *arguments], text=True))


def selected_package(name):
    return (any(triple in name for triple, _, _ in PLATFORMS.values()) or
            any(name.endswith(arch + '-cross') for _, arch, _ in PLATFORMS.values()))


def payload_matches(archive):
    """Avoid rewriting identical installed compiler executables during setup reuse."""
    process = subprocess.Popen(['dpkg-deb', '--fsys-tarfile', str(archive)], stdout=subprocess.PIPE)
    matched, hashes = True, {}
    try:
        with tarfile.open(fileobj=process.stdout, mode='r|') as payload:
            for entry in payload:
                relative = Path(entry.name)
                if relative.is_absolute() or '..' in relative.parts:
                    raise SystemExit('Unsafe package payload path: ' + entry.name)
                target = ROOT / relative
                if entry.isfile():
                    source = payload.extractfile(entry)
                    fingerprint = hashlib.sha256()
                    for chunk in iter(lambda: source.read(1024 * 1024), b''):
                        fingerprint.update(chunk)
                    expected = fingerprint.hexdigest()
                    hashes[str(relative)] = expected
                    matched &= target.is_file() and not target.is_symlink() and sha(target) == expected
                elif entry.issym():
                    matched &= target.is_symlink() and target.readlink() == Path(entry.linkname)
                elif entry.islnk():
                    expected = hashes.get(str(Path(entry.linkname)))
                    matched &= bool(expected) and target.is_file() and sha(target) == expected
                elif entry.isdir():
                    matched &= target.is_dir()
                else:
                    matched = False
    finally:
        process.stdout.close()
    if process.wait():
        raise SystemExit('Cannot inspect verified package payload: ' + str(archive))
    return matched


def audit_packages(install):
    packages = []
    for archive in sorted((ENV / 'apt/archives').glob('*.deb')):
        record = control_paragraphs(subprocess.check_output(['dpkg-deb', '-f', str(archive)], text=True))[0]
        if not selected_package(record['Package']):
            continue
        digest = sha(archive)
        candidates = apt_records('show', record['Package'] + '=' + record['Version'])
        matched = next((candidate for candidate in candidates if candidate.get('Version') == record['Version'] and
                        candidate.get('Architecture') == record['Architecture'] and candidate.get('SHA256') == digest), None)
        if not matched:
            raise SystemExit('Package SHA256 not in authenticated APT indexes: ' + archive.name)
        if install and not payload_matches(archive):
            subprocess.run(['dpkg-deb', '--extract', str(archive), str(ROOT)], check=True)
        packages.append({'package': record['Package'], 'version': record['Version'],
                         'architecture': record['Architecture'], 'source': record.get('Source', record['Package']),
                         'built_using': record.get('Built-Using', ''), 'archive': str(archive),
                         'sha256': digest, 'index_filename': matched.get('Filename')})
    return packages


def audit_source():
    record = next((item for item in apt_records('showsrc', 'glibc') if item.get('Version') == '2.41-11'), None)
    if not record:
        raise SystemExit('Exact glibc 2.41-11 source is absent from authenticated Sources index')
    artifacts = []
    for line in record['Checksums-Sha256'].splitlines():
        if not line.strip():
            continue
        digest, size, name = line.split()
        path = ENV / 'glibc-source' / name
        if path.stat().st_size != int(size) or sha(path) != digest:
            raise SystemExit('Source artifact verification failed: ' + name)
        artifacts.append({'file': str(path), 'bytes': int(size), 'sha256': digest})
    return {'package': 'glibc', 'version': '2.41-11', 'artifacts': artifacts}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--install', action='store_true', help='Extract verified cross packages into workspace tool root')
    args = parser.parse_args()
    packages, source = audit_packages(args.install), audit_source()
    targets = {}
    for platform, (triple, architecture, loader) in PLATFORMS.items():
        library_root = ROOT / 'usr' / triple / 'lib'
        libraries = {}
        for name in (loader, 'libc.so.6', 'libm.so.6', 'libpthread.so.0', 'libdl.so.2', 'librt.so.1',
                     'libresolv.so.2', 'libutil.so.1', 'libanl.so.1', 'libnss_dns.so.2', 'libnss_files.so.2', 'libgcc_s.so.1'):
            path = library_root / name
            if path.is_file() and path.read_bytes().startswith(b'\x7fELF'):
                libraries['/lib/' + name] = {'file': str(path), 'sha256': sha(path), 'bytes': path.stat().st_size}
        if '/lib/' + loader not in libraries or '/lib/libc.so.6' not in libraries:
            raise SystemExit('Missing actual glibc loader/runtime for ' + platform)
        runtime = next(item for item in packages if item['package'] == 'libc6-' + architecture + '-cross')
        if 'glibc (= 2.41-11)' not in runtime['built_using']:
            raise SystemExit('Cross runtime does not match the audited exact glibc source')
        targets[platform] = {'compiler': compiler_identity(str(ROOT / 'usr/bin' / (triple + '-gcc'))),
                             'triple': triple, 'sysroot': str(ROOT), 'include': str(ROOT / 'usr' / triple / 'include'),
                             'interpreter': '/lib/' + loader, 'glibc_version': runtime['version'],
                             'libraries': libraries}
    manifest = {'schema_version': 1, 'distribution': 'Debian trixie', 'glibc_source': source,
                'trust': {'signed_by': '/usr/share/keyrings/debian-archive-keyring.gpg',
                          'keyring_sha256': sha('/usr/share/keyrings/debian-archive-keyring.gpg'),
                          'inrelease_sha256': {p.name: sha(p) for p in (ENV / 'apt/lists').glob('*InRelease')},
                          'verification': 'APT signature verification and package/source SHA256 from authenticated indexes'},
                'packages': packages, 'platforms': targets}
    destination = ENV / 'glibc/manifest.json'
    atomic_write_changed(destination, json.dumps(manifest, indent=2, sort_keys=True) + '\n')
    for platform, item in targets.items():
        print(platform + ': glibc ' + item['glibc_version'] + ' interpreter ' + item['interpreter'])
    print(destination)


if __name__ == '__main__':
    main()
