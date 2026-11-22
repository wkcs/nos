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
    parser.add_argument("--log", type=Path, help="Optional raw UART log output path")
    args = parser.parse_args()

    qemu_bin = shutil.which("qemu-system-aarch64")
    if qemu_bin is None:
        print("SKIP: qemu-system-aarch64 not installed in PATH", file=sys.stderr)
        sys.exit(2)

    elf = args.elf.resolve()
    machine = f"virt,gic-version={args.gic_version}"
    command = [
        qemu_bin,
        "-M", machine,
        "-cpu", args.cpu,
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
        wait_for(b"ARM64 GIC initialized")
        wait_for(b"vfs_smoke OK")
        wait_for(b"nos# ")

        for cmd, expected in (
            (b"help\r", b"Available commands:"),
            (b"ls /\r", b"dev"),
            (b"ps\r", b"idel"),
            (b"free\r", b"total"),
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
