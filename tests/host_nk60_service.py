#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Sanitize actual keyboard service glue."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
INPUTS = ['tests/host_nk60_service.c', 'servers/keyboard/service.c',
          'servers/keyboard/service.h', 'servers/keyboard/keyboard.c',
          'servers/keyboard/keyboard.h', 'servers/keyboard/keycodes.h',
          'servers/keyboard/keymap.c', 'servers/keyboard/config.c',
          'servers/keyboard/config.h', 'servers/usb/keyboard_usb.c',
          'servers/usb/keyboard_usb.h', 'boards/nk60_v2/hardware.h',
          'boards/nk60_v2/nk60_usb.h']
REQUIRED = {
    'nk60_service_real_engine_usb_press_release',
    'nk60_service_usb_backpressure_retains_release',
    'nk60_service_physical_unlock_parser_and_nonblocking_uart',
    'nk60_service_flash_progress_keeps_usb_and_scan_alive',
    'nk60_service_suspend_rgb_budget_and_microsecond_wrap',
    'nk60_service_uart_macro_configuration_executes',
}


def main():
    out = ROOT / 'out/microkernel/host-nk60-service' / str(time.time_ns())
    out.mkdir(parents=True)
    hashes = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in INPUTS}
    command = ['gcc', '-Iinclude/nos', '-Wall', '-Wextra', '-Werror', '-std=gnu11',
               '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               'tests/host_nk60_service.c', 'servers/keyboard/service.c',
               'servers/keyboard/keyboard.c', 'servers/keyboard/keymap.c',
               'servers/keyboard/config.c', 'servers/usb/keyboard_usb.c',
               '-o', str(out / 'host-nk60-service')]
    with (out / 'build.log').open('w') as stream:
        build = subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT)
    code = None
    if not build.returncode:
        environment = os.environ.copy()
        environment.update(ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
                           UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        with (out / 'host.log').open('w') as stream:
            try:
                run = subprocess.run([str(out / 'host-nk60-service')], cwd=ROOT, env=environment,
                                     stdout=stream, stderr=subprocess.STDOUT, timeout=60)
                code = run.returncode
            except subprocess.TimeoutExpired:
                pass
    text = (out / 'host.log').read_text() if (out / 'host.log').exists() else ''
    checks = {line[5:] for line in text.splitlines() if line.startswith('PASS ')}
    stable = hashes == {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in INPUTS}
    good = build.returncode == 0 and code == 0 and stable and REQUIRED <= checks and 'FAIL' not in text
    result = {'passed': good, 'scope': 'Actual service, debounce/layer engine, NOR journal and HID core with modeled physical drivers and host transport; no electrical or USB enumeration claim',
              'build_command': command, 'build_returncode': build.returncode,
              'returncode': code, 'required': sorted(REQUIRED), 'missing': sorted(REQUIRED - checks),
              'source_stable': stable, 'source_sha256': hashes,
              'sanitizers': ['address', 'undefined'], 'leak_detection': True}
    (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))
    if not good:
        print((out / 'build.log').read_text()[-5000:] if build.returncode else text[-5000:])
    print(out / 'results.json')
    return 0 if good else 1


if __name__ == '__main__':
    raise SystemExit(main())
