#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Sanitize USB protocol and DWC2 transport."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
INPUTS = ["tests/host_usb_keyboard.c", "servers/usb/keyboard_usb.h",
          "servers/usb/keyboard_usb.c", "boards/nk60_v2/nk60_usb.h",
          "boards/nk60_v2/usb.c", "boards/nk60_v2/hardware.h"]
REQUIRED = {
    "usb_ep0_descriptor_layout_setup_cancel_address_led_stall",
    "usb_boot_nkro_media_queue_backpressure_release_halt",
    "usb_per_report_idle_suspend_host_optin_wake_wrap",
    "usb_dwc2_gpio_clock_fifo_w1c_setup_out_completion_timeout",
    "usb_10000_malformed_setup_and_out_sequences",
}


def main():
    out = ROOT / "out/microkernel/host-usb-keyboard" / str(time.time_ns())
    out.mkdir(parents=True)
    hashes = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in INPUTS}
    command = [os.environ.get("CC", "gcc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
               "-g", "-O1", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
               "-fno-pie", "-no-pie", "tests/host_usb_keyboard.c",
               "servers/usb/keyboard_usb.c", "boards/nk60_v2/usb.c", "-o", str(out / "test")]
    with (out / "build.log").open("w") as stream:
        build = subprocess.run(command, check=False, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT)
    code = None
    if not build.returncode:
        environment = os.environ.copy()
        environment.update(ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
                           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        with (out / "host.log").open("w") as stream:
            try:
                code = subprocess.run([str(out / "test")], check=False, cwd=ROOT, env=environment,
                                      stdout=stream, stderr=subprocess.STDOUT, timeout=60).returncode
            except subprocess.TimeoutExpired:
                pass
    text = (out / "host.log").read_text() if (out / "host.log").exists() else ""
    checks = {line[5:] for line in text.splitlines() if line.startswith("PASS ")}
    stable = hashes == {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in INPUTS}
    good = build.returncode == 0 and code == 0 and stable and REQUIRED <= checks and "FAIL" not in text
    result = {"passed": good,
              "scope": "Actual freestanding USB keyboard protocol and STM32 DWC2 driver with injected transport/register model; no physical enumeration, electrical, clock or interoperability validation",
              "build_command": command, "build_returncode": build.returncode, "returncode": code,
              "required": sorted(REQUIRED), "missing": sorted(REQUIRED - checks),
              "source_stable": stable, "source_sha256": hashes,
              "sanitizers": ["address", "undefined", "leak"]}
    (out / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))
    if not good:
        print((out / "build.log").read_text()[-5000:] if build.returncode else text[-5000:])
    print(out / "results.json")
    return 0 if good else 1


if __name__ == "__main__":
    raise SystemExit(main())
