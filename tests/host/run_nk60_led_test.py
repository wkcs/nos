#!/usr/bin/env python3
"""Exercise NK60 WS2812B waveform and DMA lifecycle using the actual driver."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
headers = ["kernel/" + name + ".h" for name in
           ("task", "errno", "sleep", "cpu", "irq", "device", "init", "gpio",
            "spinlock", "mm", "mutex", "clk")]
headers += ["board/board.h", "nos/notification.h", "nos/timeout.h", "nos/display.h"]
with tempfile.TemporaryDirectory(prefix="nk60-led-test-") as tmp:
    inc = Path(tmp)
    for name in headers:
        path = inc / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "nk60_led_test_platform.h"\n')
    exe = inc / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra",
                    "-Werror", "-Wno-unused-parameter", "-Wno-unused-function", "-Wno-unused-variable",
                    "-Wno-pointer-to-int-cast", "-I", tmp,
                    "-I", str(root / "tests/host"), "-I", str(root / "include"),
                    str(root / "tests/host/test_nk60_led.c"), "-o", str(exe)], check=True)
    results = [subprocess.run([str(exe), case]).returncode
               for case in ("waveform", "frame", "reset", "completion", "error", "fifo",
                            "direct", "combined", "timeout", "spurious", "disabled",
                            "clock", "stuck", "stale")]
    if any(results):
        raise SystemExit(1)
