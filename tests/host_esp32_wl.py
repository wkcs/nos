#!/usr/bin/env python3
# Copyright (C) 2026 NOS contributors.
"""Original pinned IDF wear levelling with NOS guards and a faulting NOR model."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import esp32s3 as port


def main():
    port.verify_sdk()
    sdk = port.IDF / "components/wear_levelling"
    sources = [
        ROOT / "ports/esp32/components/nos_devices/flash_wl.cpp",
        ROOT / "tests/host/esp32_flash/wl_regression.cpp",
        *[sdk / n for n in ("WL_Flash.cpp", "Partition.cpp", "crc32.cpp")],
    ]
    out = ROOT / "out/esp32-wl-model" / str(time.time_ns())
    out.mkdir(parents=True)
    command = [
        "c++",
        "-std=c++17",
        "-O1",
        "-g",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
        "-fno-pie",
        "-no-pie",
        *[
            "-I" + str(p)
            for p in (
                ROOT / "tests/host/esp32_flash",
                ROOT / "ports/esp32/components/nos_devices",
                ROOT / "include",
                sdk / "private_include",
                sdk,
            )
        ],
        *map(str, sources),
        "-o",
        str(out / "regression"),
    ]
    subprocess.run(command, check=True)
    result = subprocess.run(
        [out / "regression"],
        capture_output=True,
        text=True,
        env=dict(
            os.environ,
            ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
            UBSAN_OPTIONS="halt_on_error=1",
        ),
    )
    (out / "run.log").write_text(result.stdout + result.stderr)
    (out / "results.json").write_text(
        json.dumps(
            {
                "passed": result.returncode == 0,
                "exit_code": result.returncode,
                "sanitizers": ["address", "undefined", "leak"],
                "inputs": {
                    str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in [
                        *sources,
                        Path(__file__),
                        *sorted((sdk / "private_include").glob("*.h")),
                        *sorted((ROOT / "tests/host/esp32_flash").glob("*.h")),
                        ROOT / "ports/esp32/components/nos_devices/flash_block.h",
                    ]
                },
                "scope": "unmodified pinned SDK WL/Partition/CRC classes + actual NOS guard, simulated NOR/ROM boundary",
            },
            indent=2,
        )
        + "\n"
    )
    print(result.stdout + result.stderr, end="")
    print(out / "results.json")
    result.check_returncode()


if __name__ == "__main__":
    main()
