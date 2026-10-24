#!/usr/bin/env python3
# Copyright (C) 2026 NOS contributors.
"""Fault-inject SDK calls in the actual Wi-Fi adapter and NOS service manager."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    out = args.output or ROOT / "out/esp32s3/wireless-host" / str(time.time_ns())
    out = out.resolve()
    out.mkdir(parents=True)
    source = ROOT / "ports/esp_idf/components/nos_devices/wireless.c"
    # Keep every Wi-Fi statement verbatim; replace includes with the SDK fault
    # model. The real Xtensa build separately checks official SDK declarations.
    original = source.read_text()
    wifi = original
    wifi = re.sub(r'^#include "[^\n]+"\n', "", wifi, flags=re.M)
    (out / "wifi.c").write_text('#include "host_esp32s3_wifi_sdk.h"\n' + wifi)
    inputs = [
        source,
        ROOT / "tests/host_esp32s3_wifi.c",
        ROOT / "tests/host_esp32s3_wifi_sdk.h",
        ROOT / "servers/drivers/framework/service.c",
        ROOT / "servers/drivers/framework/device.c",
    ]
    hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    subprocess.run(
        [
            "gcc",
            "-Iinclude",
            "-Iinclude/nos",
            "-Itests",
            "-std=gnu11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-g",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            "-fno-pie",
            "-no-pie",
            str(out / "wifi.c"),
            "tests/host_esp32s3_wifi.c",
            "servers/drivers/framework/service.c",
            "servers/drivers/framework/device.c",
            "-o",
            str(out / "test"),
        ],
        cwd=ROOT,
        check=True,
    )
    env = dict(
        os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1", UBSAN_OPTIONS="halt_on_error=1"
    )
    subprocess.run([out / "test"], env=env, cwd=ROOT, check=True, timeout=30)
    assert hashes == {
        str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs
    }
    (out / "results.json").write_text(
        json.dumps(
            {
                "passed": True,
                "source_sha256": hashes,
                "scope": "Actual Wi-Fi adapter with SDK fault model; not radio hardware or BLE runtime",
                "sanitizers": ["address", "undefined", "leak"],
            },
            indent=2,
        )
        + "\n"
    )
    print(out / "results.json")


if __name__ == "__main__":
    main()
