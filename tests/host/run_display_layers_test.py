#!/usr/bin/env python3
"""Run display composition and configurable keyboard effect regressions."""
from pathlib import Path
import os
import subprocess
import tempfile


root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="nos-display-layers-") as tmp:
    exe = Path(tmp) / "test_display_layers"
    subprocess.run([
        os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-Wall", "-Wextra",
        "-Wno-unused-function", "-fsanitize=address,undefined",
        "-DCONFIG_SYS_TICK_MS=1", '-DCONFIG_LED_DEV="nk60_v2-led"',
        "-I", str(root / "tests/host/stubs"), "-I", str(root / "include"),
        str(root / "tests/host/test_display_layers.c"),
        str(root / "app/server/keyboard/definitions/effects/static.c"),
        str(root / "app/server/keyboard/definitions/effects/breath.c"), "-o", str(exe),
    ], check=True)
    subprocess.run([str(exe)], check=True)
    print("display layer composition and action-controlled lighting checks passed")
