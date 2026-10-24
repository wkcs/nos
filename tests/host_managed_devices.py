#!/usr/bin/env python3
# Copyright (C) 2026 NOS contributors.
"""Run the actual portable service manager with injected hardware failures."""
import os
import hashlib
import json
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    out = ROOT / "out/microkernel/host-managed-devices" / str(time.time_ns())
    out.mkdir(parents=True)
    inputs = [
        ROOT / path
        for path in (
            "tests/host_managed_devices.py",
            "tests/host_managed_devices.c",
            "servers/drivers/framework/device.c",
            "servers/drivers/framework/service.c",
            "include/nos/driver/device.h",
            "include/nos/driver/service.h",
            "scripts/check_idf_app_devices.py",
        )
    ]
    hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    subprocess.run(
        [
            "gcc",
            "-Iinclude/nos",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-g",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            "-fno-pie",
            "-no-pie",
            "-pthread",
            "tests/host_managed_devices.c",
            "servers/drivers/framework/device.c",
            "servers/drivers/framework/service.c",
            "-o",
            str(out / "test"),
        ],
        cwd=ROOT,
        check=True,
    )
    env = dict(
        os.environ,
        ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
        UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
    )
    subprocess.run([out / "test"], cwd=ROOT, env=env, check=True, timeout=30)
    policy = ROOT / "scripts/check_idf_app_devices.py"
    sample = out / "policy.c"
    for header in ("driver/gpio.h", "hal/spi_ll.h", "esp_wifi.h", "nvs_flash.h", "tinyusb.h"):
        sample.write_text(f'#include "{header}"\n')
        result = subprocess.run(["python3", policy, sample], capture_output=True, text=True)
        assert result.returncode == 1 and "use NOS devices" in result.stderr
    sample.write_text('#include "nos/driver/service_io.h"\n')
    subprocess.run(["python3", policy, sample], check=True)
    assert hashes == {
        str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs
    }
    report = out / "results.json"
    report.write_text(
        json.dumps(
            {
                "passed": True,
                "source_sha256": hashes,
                "sanitizers": ["address", "undefined", "leak"],
                "threads": 4,
                "calls": 4000,
                "application_header_policy": True,
                "hardware_verified": False,
            },
            indent=2,
        )
        + "\n"
    )
    print(report)


if __name__ == "__main__":
    main()
