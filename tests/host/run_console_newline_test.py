#!/usr/bin/env python3
"""Exercise serial console newline conversion and raw UART semantics."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="nos-console-newline-") as tmp:
    exe = Path(tmp) / "test"
    for flags in (["-DCONFIG_CONSOLE_CRLF"], []):
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", *flags,
                        "-DCONFIG_LOG_MESSAGE_SIZE=32", "-I", str(root / "tests/host/stubs"),
                        "-I", str(root / "include"), str(root / "tests/host/test_console_newline.c"),
                        "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
