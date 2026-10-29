#!/usr/bin/env python3
"""Check the shell stream primitive on scheduled QEMU tasks."""
from pathlib import Path
import argparse
import os
import selectors
import signal
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument("elf", type=Path)
args = parser.parse_args()
elf = args.elf.resolve()
proc = subprocess.Popen(["qemu-system-arm", "-M", "netduinoplus2", "-nographic",
                         "-semihosting", "-semihosting-config", "enable=on,target=native",
                         "-kernel", str(elf)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, start_new_session=True, cwd=elf.parent)
selector = selectors.DefaultSelector()
selector.register(proc.stdout, selectors.EVENT_READ)
output = bytearray()
try:
    deadline = time.monotonic() + 12
    while b"shell stream pipe OK" not in output:
        if b"shell stream pipe FAILED" in output:
            raise RuntimeError("kernel stream pipe test failed")
        if time.monotonic() >= deadline:
            raise RuntimeError("timed out waiting for kernel stream pipe test")
        for key, _ in selector.select(0.1):
            data = os.read(key.fd, 8192)
            if data:
                output.extend(data)
            elif proc.poll() is not None:
                raise RuntimeError(f"QEMU exited with {proc.returncode}")
    print("QEMU shell stream pipe concurrency passed")
finally:
    if proc.poll() is None:
        os.killpg(proc.pid, signal.SIGTERM)
        proc.wait(timeout=3)
    selector.close()
