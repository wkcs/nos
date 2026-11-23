#!/usr/bin/env python3
"""QEMU ARM64 Virt 自动化冒烟测试（验证 Higher-Half MMU、GICv3/v2、Timer 与 nShell 交互）。"""

from pathlib import Path
import argparse
import os
import selectors
import shutil
import signal
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description="QEMU ARM64 Virt Smoke Test")
    parser.add_argument("elf", type=Path, help="Path to nos.elf")
    parser.add_argument("--gic-version", choices=["2", "3"], default="3")
    parser.add_argument("--cpu", default="cortex-a53")
    parser.add_argument("--smp", default="4")
    parser.add_argument("--iommu", choices=["none", "smmuv3"], default="none")
    parser.add_argument("--mte", action="store_true")
    parser.add_argument("--log", type=Path, help="Optional raw UART log output path")
    args = parser.parse_args()

    qemu_bin = shutil.which("qemu-system-aarch64")
    if qemu_bin is None:
        print("SKIP: qemu-system-aarch64 not installed in PATH", file=sys.stderr)
        sys.exit(2)

    elf = args.elf.resolve()
    machine = f"virt,gic-version={args.gic_version}"
    if args.iommu != "none":
        machine += f",iommu={args.iommu}"
    if args.mte:
        machine += ",mte=on"
    command = [
        qemu_bin,
        "-M", machine,
        "-cpu", args.cpu,
        "-smp", str(args.smp),
        "-m", "512M",
        "-nographic",
        "-kernel", str(elf),
    ]
    proc = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        start_new_session=True,
        cwd=elf.parent,
    )
    selector = selectors.DefaultSelector()
    selector.register(proc.stdout, selectors.EVENT_READ)
    output = bytearray()

    def wait_for(needle, start=0, timeout=10):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if needle in output[start:]:
                return
            for key, _ in selector.select(0.1):
                data = os.read(key.fd, 8192)
                if data:
                    output.extend(data)
                elif proc.poll() is not None:
                    raise RuntimeError(
                        f"QEMU exited early ({proc.returncode}): {output[start:]!r}"
                    )
        raise RuntimeError(
            f"timed out waiting for {needle!r}; captured: {output[start:]!r}"
        )

    try:
        wait_for(b"Welcome to NOS:")
        wait_for(b"ARM64 CPU:")
        wait_for(b"ARM64 ISA:")
        wait_for(b"ARM64 Security:")
        wait_for(b"ARM64 GIC initialized")
        wait_for(b"vfs_smoke OK")
        wait_for(b"ARM64 SMP:")
        wait_for(b"[EL0] Hello via SVC #0!")
        wait_for(b"ARM64 Stage2A selftest PASS")
        wait_for(b"[EL0-CELL] Telemetry & Sealed VMO OK!")
        wait_for(b"ARM64 Stage2B selftest PASS")
        wait_for(b"[EL0-STAGE3] RingIPC, NEON Lazy-Ctx & SchedInfer OK!")
        wait_for(b"ARM64 Stage3 selftest PASS")
        wait_for(b"[EL0-STAGE4] SMMUv3 SVA, AccelQueue GEMV & ARMv9 Sec OK!")
        wait_for(b"ARM64 Stage4 selftest PASS")
        wait_for(b"[EL0-ELF64] C Agent Cell (libnos SDK): topo+accel GEMV=[16,40] OK!")
        wait_for(b"ARM64 Stage5 selftest PASS")
        if args.cpu == "max":
            wait_for(b"sve_lazy_ctx=ok")
        if args.iommu == "smmuv3":
            wait_for(b"ARM64 SMMUv3 hardware active")
            wait_for(b"smmuv3=hw")
        if args.mte and args.cpu == "max":
            wait_for(b"ARM64 Security: PAC=on BTI=on MTE=on")
            wait_for(b"sec_pac=1, sec_bti=1, sec_mte=1")
        wait_for(b"nos# ")

        for cmd, expected in (
            (b"help\r", b"Available commands:"),
            (b"ver\r", b"NOS Kernel"),
            (b"lsdev\r", b"UART1"),
            (b"ls /\r", b"dev"),
            (b"ai\r", b"AccelQueue:"),
            (b"runelf /bin/ai_agent.elf\r", b"exit=0"),
            (b"ps\r", b"idel"),
            (b"free\r", b"total"),
            (b"sleep 1\r", b"sleep 1"),
            (b"uptime\r", b"up 1."),
        ):
            pos = len(output)
            proc.stdin.write(cmd)
            proc.stdin.flush()
            wait_for(expected, pos)
            wait_for(b"nos# ", pos)
    finally:
        if args.log is not None:
            args.log.write_bytes(output)
        selector.close()
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait(timeout=5)

    print("qemu-arm64 smoke checks passed")


if __name__ == "__main__":
    main()
