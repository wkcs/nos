#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Sanitize the actual nshell parser and editor."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
INPUTS = ['tests/host_shell.c', 'include/nos/shell.h',
          'user/shell/parser.c', 'user/shell/editor.c']
REQUIRED = {
    'shell_parser_quotes_escapes_empty_and_limits',
    'shell_parser_bounded_fuzz_100000',
    'shell_parser_quoted_roundtrip_10000',
    'shell_editor_insert_delete_navigation_history_crlf_cancel',
    'shell_editor_history_bound_overflow_and_recovery',
    'shell_editor_bounded_fuzz_100000',
}


def main():
    out = ROOT / 'out/microkernel/host-shell' / str(time.time_ns())
    out.mkdir(parents=True)
    hashes = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in INPUTS}
    command = ['gcc', '-Iinclude/nos', '-Wall', '-Wextra', '-Werror', '-std=gnu11',
               '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               'tests/host_shell.c', 'user/shell/parser.c', 'user/shell/editor.c',
               '-o', str(out / 'host-shell')]
    with (out / 'build.log').open('w') as stream:
        build = subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT)
    code = None
    if not build.returncode:
        environment = os.environ.copy()
        environment.update(ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
                           UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        with (out / 'host.log').open('w') as stream:
            try:
                run = subprocess.run([str(out / 'host-shell')], cwd=ROOT, env=environment,
                                     stdout=stream, stderr=subprocess.STDOUT, timeout=60)
                code = run.returncode
            except subprocess.TimeoutExpired:
                pass
    text = (out / 'host.log').read_text() if (out / 'host.log').exists() else ''
    checks = {line[5:] for line in text.splitlines() if line.startswith('PASS ')}
    stable = hashes == {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in INPUTS}
    good = build.returncode == 0 and code == 0 and stable and REQUIRED <= checks and 'FAIL' not in text
    result = {'passed': good, 'scope': 'Actual bounded nshell parser/editor; host output callback does not validate UART, VFS or application execution',
              'build_command': command, 'build_returncode': build.returncode,
              'returncode': code, 'required': sorted(REQUIRED), 'missing': sorted(REQUIRED - checks),
              'source_stable': stable, 'source_sha256': hashes,
              'fuzz_seed': '0x51e11', 'parser_iterations': 100000,
              'quoted_roundtrip_iterations': 10000, 'editor_iterations': 100000,
              'sanitizers': ['address', 'undefined']}
    (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))
    if not good:
        print((out / 'build.log').read_text()[-5000:] if build.returncode else text[-5000:])
    print(out / 'results.json')
    return 0 if good else 1


if __name__ == '__main__':
    raise SystemExit(main())
