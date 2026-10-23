#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Exercise the real bounded keyboard engine."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
INPUTS = ['tests/host_keyboard.c', 'tests/host_keyboard.py'] + [
    'servers/keyboard/' + name for name in
    ['keyboard.c', 'keyboard.h', 'keymap.c', 'keycodes.h', 'config.c', 'config.h']]
MARKERS = ['keyboard_symmetric_debounce_wrap', 'keyboard_layer_binding_transparent_toggle',
           'keyboard_rollover_duplicate_modifier', 'keyboard_tap_hold_oneshot',
           'keyboard_consumer_macro_backpressure_local', 'keyboard_queue_fail_closed_atomic_remap',
           'keyboard_config_crc_dual_slot_fault_atomicity', 'keyboard_stateful_fuzz_100000',
           'keyboard_all']


def main():
    out = ROOT / 'out/microkernel/host-keyboard' / str(time.time_ns())
    out.mkdir(parents=True)
    command = ['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
               'tests/host_keyboard.c', 'servers/keyboard/keyboard.c',
               'servers/keyboard/keymap.c', 'servers/keyboard/config.c', '-o', str(out / 'keyboard')]
    with (out / 'build.log').open('w') as log:
        build = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    code = None
    if build.returncode == 0:
        env = os.environ.copy()
        env.update(ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
                   UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        with (out / 'host.log').open('w') as log:
            try:
                code = subprocess.run([str(out / 'keyboard')], env=env, cwd=ROOT,
                                      stdout=log, stderr=subprocess.STDOUT, timeout=90).returncode
            except subprocess.TimeoutExpired:
                pass
    text = (out / 'host.log').read_text() if (out / 'host.log').exists() else ''
    result = {'passed': build.returncode == 0 and code == 0 and 'FAIL' not in text and
              all('PASS ' + marker in text for marker in MARKERS),
              'scope': 'real portable keyboard engine and fake NOR faults; no GPIO, USB electrical or real-board verification',
              'build_returncode': build.returncode, 'returncode': code,
              'required_markers': MARKERS, 'iterations': 100000, 'fuzz_seed': '0x1badb002',
              'sanitizers': ['address', 'undefined'],
              'compiler': subprocess.check_output(['gcc', '--version'], text=True).splitlines()[0],
              'source_sha256': {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in INPUTS}}
    (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(text if not build.returncode else (out / 'build.log').read_text())
    print(out / 'results.json')
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
