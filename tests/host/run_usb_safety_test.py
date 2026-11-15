#!/usr/bin/env python3
"""Test USB safety, tail packet handling, ZLP state machine, and bounds on a host."""

from pathlib import Path
import os
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"


def main():
    with tempfile.TemporaryDirectory(prefix="nos-usb-safety-test-") as tmp:
        exe = Path(tmp) / "test_usb_safety"
        cmd = [
            os.environ.get("CC", "cc"),
            "-std=gnu11",
            "-Wno-incompatible-function-pointer-types",
            "-I", str(TEST_DIR / "stubs"),
            "-I", str(ROOT / "include"),
            "-I", str(ROOT),
            "-DCONFIG_SYS_TICK_MS=1",
            str(TEST_DIR / "test_usb_safety.c"),
            "-o", str(exe),
        ]
        if sys.platform == "darwin":
            cmd.extend(["-Wl,-no_fixup_chains", "-Wl,-w"])
        subprocess.run(
            cmd,
            check=True,
        )
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
