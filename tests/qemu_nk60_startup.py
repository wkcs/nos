#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. NK60 CPU/ABI startup, with RCC init explicitly skipped.

Stock netduinoplus2 models a different STM32. This test uses the actual NK60
firmware but a debugger skips nk60_boot_init; it proves Cortex-M startup,
MPU setup and syscall dispatch only. It does not emulate or validate NK60
clocks, USB, shift registers, SPI flash, LEDs or electrical timing.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import time

from qemu_matrix import ROOT, file_digest, git_identity, tool_version


def debugger_script(image, port, output, mpu):
    python = f'''
import gdb, json
snapshots = {{}}
def value(expression):
    return int(gdb.parse_and_eval(expression))
def word(address):
    return value('*(unsigned*)%d' % address)
def checkpoint(name):
    task = 'cpu_tasks[0]'
    data = dict(pc=value('$pc'), control=value('$control'), psp=value('$psp'),
                msp=value('$msp'), sp=value('$sp'), xpsr=value('$xpsr'),
                task_id=value(task + '->id'), caps=value(task + '->caps'),
                memory=value(task + '->memory'), mpu_ctrl=mpu_ctrl)
    assert data['control'] == 3, 'not unprivileged PSP Thread mode'
    assert data['xpsr'] & 0x1ff == 0, 'not Thread mode'
    assert data['psp'] == data['sp'], 'Thread stack is not PSP'
    assert data['memory'] <= data['psp'] < data['memory'] + 4096
    assert 0x20000000 <= data['memory'] <= 0x20010000 - 4096
    assert value('(unsigned)&__user_bss_end') < data['msp'] <= 0x20010000
    assert data['mpu_ctrl'] & 5 == {5 if mpu else 0}, 'MPU enable state mismatch'
    snapshots[name] = data
    return data
snapshots['reset_entry'] = dict(pc=value('$pc'), msp=value('$msp'), control=value('$control'),
    vector_stack=word(0x08000000), vector_reset=word(0x08000004))
assert snapshots['reset_entry']['pc'] == value('(unsigned)reset_handler') & ~1
assert snapshots['reset_entry']['vector_reset'] == snapshots['reset_entry']['pc'] | 1
assert snapshots['reset_entry']['msp'] == snapshots['reset_entry']['vector_stack'] == 0x20010000
assert snapshots['reset_entry']['control'] == 0
# Break at the raw function entry, before its prologue. Only the peripheral
# clock/GPIO/TIM2 init call is skipped; no firmware instruction or RAM is patched.
boot = gdb.Breakpoint('*nk60_boot_init')
gdb.execute('continue')
assert value('$pc') == value('(unsigned)nk60_boot_init') & ~1
snapshots['peripheral_stub'] = dict(function='nk60_boot_init', pc=value('$pc'),
                                    return_pc=value('$lr') & ~1)
gdb.execute('set $pc = $lr & ~1')
boot.delete()
user_switch = gdb.Breakpoint('*arch_enter_user')
gdb.execute('continue')
assert value('$pc') == value('(unsigned)arch_enter_user') & ~1
gdb.execute('maintenance packet Qqemu.PhyMemMode:0')
mpu_ctrl = word(0xe000ed94)
snapshots['privileged_mpu_state'] = dict(mpu_ctrl=mpu_ctrl, control=value('$control'), xpsr=value('$xpsr'))
user_switch.delete()
gdb.execute('maintenance packet Qqemu.PhyMemMode:1')
entry = gdb.Breakpoint('*nos_keyboard_service')
gdb.execute('continue')
assert value('$pc') == value('(unsigned)nos_keyboard_service') & ~1
service = checkpoint('keyboard_service_entry')
assert service['task_id'] == 0
assert service['caps'] & 32, 'device service lacks DEVICE capability'
assert value('(unsigned)&__stack_top') == 0x20010000
pool = value('(unsigned)&page_pool')
assert pool >= 0x20000000 and pool + 4 * 4096 <= 0x20010000
snapshots['sram_layout'] = dict(page_pool=pool, page_pool_bytes=value('sizeof(page_pool)'),
    user_data_start=value('(unsigned)&__user_data_start'),
    user_bss_end=value('(unsigned)&__user_bss_end'), stack_top=value('(unsigned)&__stack_top'))
assert snapshots['sram_layout']['page_pool_bytes'] == 4 * 4096
entry.delete()
# Redirect registers/PC to the firmware's existing nos_syscall entry. The SVC,
# exception frame, kernel dispatch and exception return all execute real code.
# This is debugger-driven ABI probing, not execution of keyboard peripherals.
start = value('(unsigned)nos_syscall') & ~1
instructions = gdb.selected_frame().architecture().disassemble(start, start + 24)
svcs = [item for item in instructions if item['asm'].split()[0] == 'svc']
assert svcs, 'actual nos_syscall has no SVC instruction'
svc = svcs[0]['addr']
assert bytes(gdb.selected_inferior().read_memory(svc, 2)) == bytes([0, 223])
returned = gdb.Breakpoint('*%d' % (svc + 2))
trap = gdb.Breakpoint('*kernel_trap')
trap.condition = '$r1 == 0'
def enter_call(number, a=0, b=0, c=0):
    global mpu_ctrl
    for register, data in [('r0', number), ('r1', a), ('r2', b), ('r3', c), ('pc', start)]:
        gdb.execute('set $%s = %d' % (register, data))
    gdb.execute('continue')
    assert value('$pc') == value('(unsigned)kernel_trap') & ~1
    assert value('$xpsr') & 0x1ff == 11, 'SVC did not enter privileged Handler mode'
    assert value('$sp') == value('$msp'), 'kernel did not switch to MSP'
    gdb.execute('maintenance packet Qqemu.PhyMemMode:0', to_string=True)
    mpu_ctrl = word(0xe000ed94)
    assert mpu_ctrl & 5 == {5 if mpu else 0}, 'MPU state changed during SVC'
    gdb.execute('maintenance packet Qqemu.PhyMemMode:1', to_string=True)
    frame = value('$r0')
    assert word(frame + 48) == number, 'r12 syscall number was not saved in frame'
    assert word(frame + 56) == svc + 2, 'wrong SVC return PC'
    assert value('$r1') == 0
    return dict(frame=frame, xpsr=value('$xpsr'), msp=value('$msp'), mpu_ctrl=mpu_ctrl)
def call(name, number, a=0, b=0, c=0, expected=0):
    handler = enter_call(number, a, b, c)
    gdb.execute('continue')
    assert value('$pc') == svc + 2, 'not returned through real exception restore'
    actual = value('$r0') & 0xffffffff
    assert actual == expected & 0xffffffff, '%s returned %#x expected %#x' % (name, actual, expected & 0xffffffff)
    data = checkpoint(name)
    data.update(number=number, arguments=[a, b, c], result=actual, handler=handler)
    return data
call('getpid_svc_roundtrip', 1, expected=0)
call('caps_svc_roundtrip', 6, expected=service['caps'])
call('unknown_syscall_rejected', 0xffff, expected=-38)
call('mmio_core_register_denied', 39, 6, 0xe000ed08, expected=-13)
call('dma_nonuser_source_denied', 39, 10, pool + 2 * 4096, 1464, expected=-14)
call('dma_invalid_length_denied', 39, 10, service['memory'], 1, expected=-22)
call('heap_unsupported', 15, expected=-95)
# Blocking the service selects the actual idle task, whose capability set is
# empty. Probe an otherwise permitted TIM2 register without touching it:
# capability rejection must happen before the MMIO operation is dispatched.
idle = gdb.Breakpoint('*nos_idle')
enter_call(4, 10000)
gdb.execute('continue')
assert value('$pc') == value('(unsigned)nos_idle') & ~1
idle_data = checkpoint('idle_scheduler_entry')
assert idle_data['task_id'] == 3 and idle_data['caps'] == 0
idle.delete()
call('idle_device_capability_denied', 39, 6, 0x40000024, expected=-13)
with open({str(output / 'snapshots.json')!r}, 'w') as stream:
    json.dump(snapshots, stream, indent=2)
print('PASS nk60_startup_thread_psp_device_service')
print('PASS nk60_startup_mpu_state_and_sram_bounds')
print('PASS nk60_startup_real_svc_handler_roundtrip')
print('PASS nk60_startup_mmio_dma_validation')
print('PASS nk60_startup_idle_scheduler_and_device_denial')
# Stay stopped. Parent terminates QEMU after collecting these assertions.
gdb.execute('disconnect')
'''
    return '\n'.join(['set pagination off', 'set confirm off', 'set remotetimeout 5',
                      'set python print-stack full',
                      f'file {image}', f'target remote 127.0.0.1:{port}',
                      'maintenance packet Qqemu.PhyMemMode:1',
                      'python', python, 'end', 'quit']) + '\n'


REQUIRED = {
    'nk60_startup_thread_psp_device_service',
    'nk60_startup_mpu_state_and_sram_bounds',
    'nk60_startup_real_svc_handler_roundtrip',
    'nk60_startup_mmio_dma_validation',
    'nk60_startup_idle_scheduler_and_device_denial',
}


def run_image(image, mpu, output, args):
    output.mkdir(parents=True)
    digest = file_digest(image)
    debugger_code = None
    process = None
    cleanup_code = None
    alive = False
    failure = None
    command = None
    version = None
    try:
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            port = listener.getsockname()[1]
        binary = str(Path(args.qemu_bindir) / 'qemu-system-arm') if args.qemu_bindir else 'qemu-system-arm'
        command = [binary, '-M', 'netduinoplus2', '-display', 'none', '-monitor', 'none',
                   '-serial', 'stdio', '-nic', 'none', '-kernel', str(image), '-S',
                   '-gdb', f'tcp:127.0.0.1:{port}']
        if args.qemu_bindir:
            command += ['-L', str(Path(args.qemu_bindir).parent / 'share/qemu')]
        version = tool_version(binary)
        (output / 'debugger.gdb').write_text(debugger_script(image, port, output, mpu))
        with (output / 'qemu.log').open('w') as serial:
            process = subprocess.Popen(command, cwd=ROOT, stdin=subprocess.DEVNULL,
                                       stdout=serial, stderr=subprocess.STDOUT)
            with (output / 'gdb.log').open('w') as log:
                debugger = subprocess.run([args.gdb, '-q', '-batch', '-x', str(output / 'debugger.gdb')],
                                          cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                                          timeout=args.timeout)
            debugger_code = debugger.returncode
    except (OSError, subprocess.TimeoutExpired) as exc:
        failure = str(exc)
    finally:
        if process is not None:
            alive = process.poll() is None
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            cleanup_code = process.returncode
    log = (output / 'gdb.log').read_text() if (output / 'gdb.log').is_file() else ''
    serial = (output / 'qemu.log').read_text() if (output / 'qemu.log').is_file() else ''
    checks = {line[5:] for line in log.splitlines() if line.startswith('PASS ')}
    stable = image.is_file() and file_digest(image) == digest
    good = failure is None and debugger_code == 0 and alive and stable and REQUIRED <= checks and 'FAIL' not in serial
    report = {'passed': good, 'mpu': bool(mpu), 'image': str(image), 'image_sha256': digest,
              'image_stable': stable, 'gdb_returncode': debugger_code,
              'qemu_cleanup_returncode': cleanup_code, 'guest_alive_before_controlled_termination': alive,
              'failure': failure,
              'required': sorted(REQUIRED), 'missing': sorted(REQUIRED - checks),
              'qemu_command': command, 'qemu_version': version,
              'gdb_version': tool_version(args.gdb),
              'snapshots': json.loads((output / 'snapshots.json').read_text())
                  if (output / 'snapshots.json').is_file() else None}
    (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    if not good:
        print(log[-6000:])
        print(serial[-2000:])
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image-nompu', type=Path)
    parser.add_argument('--image-mpu', type=Path)
    parser.add_argument('--qemu-bindir', default=os.environ.get('QEMU_BINDIR', ''))
    parser.add_argument('--gdb', default='gdb-multiarch')
    parser.add_argument('--timeout', type=float, default=25)
    parser.add_argument('--output-root', type=Path, default=ROOT / 'out/microkernel/nk60-startup')
    args = parser.parse_args()
    if bool(args.image_nompu) != bool(args.image_mpu):
        parser.error('--image-nompu and --image-mpu must be supplied together')
    if not 0 < args.timeout <= 30:
        parser.error('--timeout must be greater than zero and at most 30 seconds')
    output = args.output_root.resolve() / str(time.time_ns())
    output.mkdir(parents=True)
    source = git_identity()
    results = []
    for mpu, provided in ((0, args.image_nompu), (1, args.image_mpu)):
        target = output / ('mpu' if mpu else 'nompu')
        target.mkdir()
        image = provided.resolve() if provided else target / 'nos.elf'
        command = None
        build_code = None
        if not provided:
            command = ['make', 'PLATFORM=nk60_v2', 'LIBC=minimal', 'BUILD_MODE=normal',
                       'SMP=1', f'MPU={mpu}', f'O={target}', 'firmware', '-j4']
            with (target / 'build.log').open('w') as log:
                try:
                    build = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=180)
                    build_code = build.returncode
                except subprocess.TimeoutExpired:
                    build_code = -1
        if image.is_file() and (provided or build_code == 0):
            case = run_image(image, mpu, target / 'run', args)
        else:
            case = {'passed': False, 'mpu': bool(mpu), 'failure': 'firmware build/image unavailable'}
        case.update(build_command=command, build_returncode=build_code,
                    image_origin='provided_image_build_source_unknown' if provided else 'built_from_reported_source')
        results.append(case)
        print(f'{"PASS" if case["passed"] else "FAIL"} nk60_startup MPU={mpu}', flush=True)
    source_after = git_identity()
    stable = source == source_after
    passed = all(case['passed'] for case in results) and (stable or bool(args.image_nompu))
    report = {'passed': passed, 'board_tested': False, 'source': source,
              'source_after': source_after, 'source_stable': stable,
              'scope': 'Actual NK60 firmware reset, Cortex-M Thread/PSP entry, MPU state, privileged SVC roundtrip, MMIO/DMA user checks and idle scheduling on stock QEMU netduinoplus2',
              'limitations': ['Debugger skips nk60_boot_init before its prologue because stock F405 QEMU does not model the F401 board clock tree.',
                  'Debugger redirects user registers/PC to existing nos_syscall code; no firmware instruction or RAM is patched.',
                  'This is CPU/kernel ABI testing, not NK60 board emulation. Clocks, USB enumeration, matrix switches, SPI flash, RGB DMA electrical output and peripheral timing remain unverified on hardware.'],
              'cases': results}
    (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': passed, 'board_tested': False, 'source_stable': stable}))
    print(output / 'results.json')
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
