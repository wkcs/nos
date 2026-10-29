#!/usr/bin/env python3
"""Check shared VFS offsets with the real scheduler and semaphore."""
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
    args = parser.parse_args()
    elf = args.elf.resolve()
    proc = subprocess.Popen(
        ["qemu-system-arm", "-M", "netduinoplus2", "-nographic",
         "-semihosting", "-semihosting-config", "enable=on,target=native",
         "-kernel", str(elf)],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        start_new_session=True, cwd=elf.parent)
    selector = selectors.DefaultSelector()
    selector.register(proc.stdout, selectors.EVENT_READ)
    output = bytearray()
    try:
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if b"vfs concurrent position OK" in output:
                print("QEMU VFS concurrent position checks passed")
                return
            if b"vfs concurrent position FAILED" in output:
                raise RuntimeError(output.decode("utf-8", errors="replace"))
            for key, _ in selector.select(0.1):
                chunk = os.read(key.fd, 8192)
                if chunk:
                    output.extend(chunk)
                elif proc.poll() is not None:
                    raise RuntimeError(f"QEMU exited with {proc.returncode}")
        raise RuntimeError("timed out waiting for VFS concurrency test:\n" +
                           output.decode("utf-8", errors="replace"))
    finally:
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGTERM)
            proc.wait(timeout=3)
        selector.close()


if __name__ == "__main__":
    main()
