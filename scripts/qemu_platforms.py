"""Copyright (C) 2026 NOS contributors. Native, unpatched QEMU targets."""
from pathlib import Path

PLATFORMS = {
    'arm_virt': ('qemu-system-arm', ['-M', 'virt,gic-version=2', '-cpu', 'cortex-a15']),
    'arm64_virt': ('qemu-system-aarch64', ['-M', 'virt,gic-version=2', '-cpu', 'cortex-a53']),
    'riscv_virt': ('qemu-system-riscv64', ['-M', 'virt', '-bios', 'none']),
    'riscv32_virt': ('qemu-system-riscv32', ['-M', 'virt', '-bios', 'none']),
    'riscv32_nommu': ('qemu-system-riscv32', ['-M', 'virt', '-bios', 'none']),
    'stm32': ('qemu-system-arm', ['-M', 'netduinoplus2']),
}
BASELINE_PLATFORMS = ('arm_virt', 'arm64_virt', 'riscv_virt', 'stm32')
PLATFORM_LIBRARIES = {
    'arm_virt': ('minimal', 'newlib'), 'stm32': ('minimal', 'newlib'),
    'arm64_virt': ('minimal', 'musl'), 'riscv_virt': ('minimal', 'musl'),
    'riscv32_virt': ('minimal',),
    'riscv32_nommu': ('minimal',),
}

def qemu_command(platform, image, bindir='', cpus=1, disk=None, extra_args=()):
    binary, flags = PLATFORMS[platform]
    if bindir:
        binary = str(Path(bindir) / binary)
    command = [binary, *flags, '-display', 'none', '-monitor', 'none', '-serial', 'stdio', '-nic', 'none', '-kernel', str(image)]
    if bindir:
        command += ['-L', str(Path(bindir).parent / 'share' / 'qemu')]
    if not platform.startswith('riscv'):
        command += ['-semihosting-config', 'enable=on,target=native']
    if platform != 'stm32':
        command += ['-m', '128M']
        command += ['-smp', str(cpus)]
    elif cpus != 1:
        raise ValueError('STM32 is a single core target')
    if disk:
        if platform == 'stm32':
            raise ValueError('The STM32 QEMU target has no virtio block controller')
        command += ['-drive', f'if=none,id=nosdisk,file={Path(disk).resolve()},format=raw',
                    '-device', 'virtio-blk-device,drive=nosdisk']
    command += list(extra_args)
    return command
