#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Exercise actual portable driver framework."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
REQUIRED = {
    "driver_16550_real_register_driver_divisor_short_io_errors_no_spin",
    "driver_lifecycle_parent_leases_reverse_rollback_pm_errors_and_bounds",
    "driver_gpio_polarity_spi_atomic_segments_reentry_i2c_restart_uart_short_usb_backpressure",
    "driver_portable_nor_independent_controller_page_verify_protect_stalled_clock",
}


def main():
    out = ROOT / "out/microkernel/host-drivers" / str(time.time_ns())
    out.mkdir(parents=True)
    sources = sorted((ROOT / "servers/drivers/framework").glob("*.c")) + [
        ROOT / "servers/drivers/spi/nor.c"
    ]
    inputs = [
        ROOT / "tests/host_driver_framework.c",
        Path(__file__).resolve(),
        *sources,
        *sorted((ROOT / "include/nos/driver").glob("*.h")),
    ]
    hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    command = [
        "gcc",
        "-Iinclude/nos",
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-O1",
        "-g",
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
        "-fno-pie",
        "-no-pie",
        "tests/host_driver_framework.c",
        *[str(p.relative_to(ROOT)) for p in sources],
        "-o",
        str(out / "test"),
    ]
    with (out / "build.log").open("w") as log:
        build = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    code = None
    if not build.returncode:
        environment = dict(
            os.environ,
            ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
            UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
        )
        with (out / "host.log").open("w") as log:
            try:
                code = subprocess.run(
                    [str(out / "test")],
                    cwd=ROOT,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    env=environment,
                    timeout=60,
                ).returncode
            except subprocess.TimeoutExpired:
                pass
    output = (out / "host.log").read_text() if (out / "host.log").exists() else ""
    checks = {line[5:] for line in output.splitlines() if line.startswith("PASS ")}
    stable = hashes == {
        str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs
    }
    passed = build.returncode == 0 and code == 0 and stable and REQUIRED <= checks
    result = {
        "passed": passed,
        "build_returncode": build.returncode,
        "returncode": code,
        "missing": sorted(REQUIRED - checks),
        "source_stable": stable,
        "source_sha256": hashes,
        "scope": "Actual user-mode device lifecycle/resources and five peripheral contracts with injected controllers; no ESP32/RK3588/hardware claim",
        "sanitizers": ["address", "undefined", "leak"],
    }
    (out / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(output if passed else (out / "build.log").read_text() + output)
    print(out / "results.json")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
