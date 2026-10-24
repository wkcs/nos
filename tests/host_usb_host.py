#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Actual USB host/OTG stack sanitizer tests."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
REQUIRED = {
    "usb_host_actual_enumeration_control_pid_bulk_partial_interrupt_stale_and_bot_recovery",
    "usb_host_descriptor_atomic_validation_50000_malformed_configs",
    "usb_otg_id_vbus_roles_no_backfeed_discharge_wrap_overcurrent_timeout_and_failed_stop",
    "usb_dwc2_dma_register_pid_nak_stall_stalled_clock_quarantine_and_confirmed_stop",
}


def main():
    out = ROOT / "out/microkernel/host-usb-host" / str(time.time_ns())
    out.mkdir(parents=True)
    sources = [
        "tests/host_usb_host.c",
        "servers/drivers/framework/device.c",
        "servers/drivers/framework/peripheral.c",
        *[str(p.relative_to(ROOT)) for p in sorted((ROOT / "servers/usb/host").glob("*.c"))],
    ]
    inputs = [
        *sources,
        "tests/host_usb_host.py",
        *[str(p.relative_to(ROOT)) for p in sorted((ROOT / "include/nos/driver").glob("*.h"))],
    ]
    hashes = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in inputs}
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
        *sources,
        "-o",
        str(out / "test"),
    ]
    with (out / "build.log").open("w") as log:
        build = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    code = None
    if build.returncode == 0:
        env = dict(
            os.environ,
            ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
            UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
        )
        with (out / "host.log").open("w") as log:
            try:
                code = subprocess.run(
                    [str(out / "test")],
                    cwd=ROOT,
                    env=env,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    timeout=90,
                ).returncode
            except subprocess.TimeoutExpired:
                pass
    output = (out / "host.log").read_text() if (out / "host.log").exists() else ""
    checks = {line[5:] for line in output.splitlines() if line.startswith("PASS ")}
    stable = hashes == {
        name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in inputs
    }
    good = build.returncode == 0 and code == 0 and stable and REQUIRED <= checks
    result = {
        "passed": good,
        "build_returncode": build.returncode,
        "returncode": code,
        "source_stable": stable,
        "source_sha256": hashes,
        "missing": sorted(REQUIRED - checks),
        "scope": "Actual USB host/enumeration/HID/BOT/OTG/DWC2 C code with adversarial protocol/register/power models; not physical hardware",
        "sanitizers": ["address", "undefined", "leak"],
    }
    (out / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(output if good else (out / "build.log").read_text() + output)
    print(out / "results.json")
    return 0 if good else 1


if __name__ == "__main__":
    raise SystemExit(main())
