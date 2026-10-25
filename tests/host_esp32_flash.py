#!/usr/bin/env python3
# Copyright (C) 2026 NOS contributors.
"""Actual ESP NOR adapter with a fault-injected SDK model, ASan/UBSan/LSan."""
from pathlib import Path
import os
import json
import hashlib
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    output = ROOT / "out/esp32-flash-model" / str(time.time_ns())
    output.mkdir(parents=True)
    binary = output / "regression"
    subprocess.run(
        [
            "cc",
            "-std=c11",
            "-g",
            "-O1",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            "-fno-pie",
            "-no-pie",
            "-I" + str(ROOT / "tests/host/esp32_flash"),
            "-I" + str(ROOT / "ports/esp32/components/nos_devices"),
            "-I" + str(ROOT / "include"),
            str(ROOT / "ports/esp32/components/nos_devices/flash_block.c"),
            str(ROOT / "tests/host/esp32_flash/regression.c"),
            "-o",
            str(binary),
        ],
        check=True,
    )
    result = subprocess.run(
        [binary],
        capture_output=True,
        text=True,
        env=dict(
            os.environ,
            ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
            UBSAN_OPTIONS="halt_on_error=1",
        ),
    )
    (output / "run.log").write_text(result.stdout + result.stderr)
    inputs = [
        Path(__file__),
        ROOT / "ports/esp32/components/nos_devices/flash_block.c",
        ROOT / "ports/esp32/components/nos_devices/flash_block.h",
        *sorted((ROOT / "tests/host/esp32_flash").glob("*")),
        *sorted((ROOT / "include/nos/driver").glob("*.h")),
    ]
    (output / "results.json").write_text(
        json.dumps(
            {
                "passed": result.returncode == 0,
                "exit_code": result.returncode,
                "sanitizers": ["address", "undefined", "leak"],
                "scope": "actual NOR backend with SDK/NOR model; no physical Flash claim",
                "inputs": {
                    str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in inputs
                    if p.is_file()
                },
            },
            indent=2,
        )
        + "\n"
    )
    print(result.stdout + result.stderr, end="")
    result.check_returncode()


if __name__ == "__main__":
    main()
