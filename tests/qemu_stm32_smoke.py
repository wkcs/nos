#!/usr/bin/env python3
"""Exercise boot, the IRQ-backed shell, and log statistics in QEMU."""

from pathlib import Path
import argparse
import os
import selectors
import signal
import subprocess
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("elf", type=Path)
    parser.add_argument("--log", type=Path)
    args = parser.parse_args()

    elf = args.elf.resolve()
    command = ["qemu-system-arm", "-M", "netduinoplus2", "-nographic",
               "-semihosting", "-semihosting-config", "enable=on,target=native",
               "-kernel", str(elf)]
    proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, start_new_session=True, cwd=elf.parent)
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
                    raise RuntimeError(f"QEMU exited with {proc.returncode}")
        raise RuntimeError(f"timed out waiting for {needle!r}")

    def wait_count(needle, count, start=0, timeout=10):
        deadline = time.monotonic() + timeout
        while output[start:].count(needle) < count:
            if time.monotonic() >= deadline:
                raise RuntimeError(f"timed out waiting for {count} occurrences of {needle!r}")
            for key, _ in selector.select(0.1):
                data = os.read(key.fd, 8192)
                if data:
                    output.extend(data)
                elif proc.poll() is not None:
                    raise RuntimeError(f"QEMU exited with {proc.returncode}")

    try:
        wait_for(b"Welcome to NOS:")
        wait_for(b"nos# ")
        pos = len(output)
        proc.stdin.write(b"help\r")
        proc.stdin.flush()
        wait_for(b"Available commands:", pos)
        wait_for(b"nos# ", pos)
        pos = len(output)
        proc.stdin.write(b"echo 'hello world'\r")
        proc.stdin.flush()
        wait_for(b"\r\nhello world\r\n", pos)
        wait_for(b"nos# ", pos)
        pos = len(output)
        proc.stdin.write(b"echo bad | echo unsafe\r")
        proc.stdin.flush()
        wait_for(b"shell: syntax error", pos)
        wait_for(b"nos# ", pos)
        pos = len(output)
        proc.stdin.write(b"false && echo no || echo yes; true && echo done\r")
        proc.stdin.flush()
        wait_for(b"\r\nyes\r\ndone\r\n", pos)
        wait_for(b"nos# ", pos)
        pos = len(output)
        proc.stdin.write(b"mkdir /tmp/one /tmp/two\r")
        proc.stdin.flush()
        wait_for(b"nos# ", pos)
        pos = len(output)
        proc.stdin.write(b"cd /tmp/two\r")
        proc.stdin.flush()
        wait_for(b"nos# ", pos)
        pos = len(output)
        proc.stdin.write(b"pwd\r")
        proc.stdin.flush()
        wait_for(b"\r\n/tmp/two\r\n", pos)
        wait_for(b"nos# ", pos)
        pos = len(output)
        proc.stdin.write(b"cat /sys/arch /sys/board\r")
        proc.stdin.flush()
        wait_for(b"arm\nqemu-stm32\n", pos)
        wait_for(b"nos# ", pos)
        pos = len(output)
        proc.stdin.write(b"ls /sys/arch\r")
        proc.stdin.flush()
        wait_for(b"\r\n/sys/arch\r\n", pos)
        wait_for(b"nos# ", pos)
        pos = len(output)
        proc.stdin.write(b"log stats\r")
        proc.stdin.flush()
        wait_for(b"dropped=", pos)
        wait_for(b"nos# ", pos)
        pos = len(output)
        proc.stdin.write(b"log stats\r" * 5)
        proc.stdin.flush()
        wait_count(b"dropped=", 5, pos)
        wait_count(b"nos# ", 5, pos)
        print("QEMU STM32 boot, help, log statistics and RX burst passed")
    finally:
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGTERM)
            proc.wait(timeout=3)
        selector.close()
        if args.log:
            args.log.write_bytes(output)


if __name__ == "__main__":
    main()
