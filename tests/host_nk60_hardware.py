#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Sanitize the actual nk60_v2 board drivers."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
INPUTS = ["tests/host_nk60_hardware.c", "boards/nk60_v2/hardware.h",
          "boards/nk60_v2/hw_internal.h", "boards/nk60_v2/hardware.c",
          "boards/nk60_v2/flash.c", "boards/nk60_v2/rgb.c"]
REQUIRED = {
    "nk60_hc595_72_slots_61_keys_first_scan_and_releases",
    "nk60_flash_jedec_page_split_sector_and_address_bounds",
    "nk60_flash_write_protect_busy_stalled_clock_spi_timeout_and_usb_progress",
    "nk60_rgb_grb_wave_dma_failure_power_off_and_canvas",
}


def main():
    out = ROOT / "out/microkernel/host-nk60-hardware" / str(time.time_ns())
    out.mkdir(parents=True)
    hashes = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in INPUTS}
    command = ["gcc", "-Iboards/nk60_v2", "-Wall", "-Wextra", "-Werror", "-std=gnu11",
               "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
               "tests/host_nk60_hardware.c", "boards/nk60_v2/hardware.c",
               "boards/nk60_v2/flash.c", "boards/nk60_v2/rgb.c", "-o", str(out / "test")]
    with (out / "build.log").open("w") as stream:
        build = subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT)
    code = None
    if not build.returncode:
        environment = os.environ.copy()
        environment.update(ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
                           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        with (out / "host.log").open("w") as stream:
            try:
                code = subprocess.run([str(out / "test")], cwd=ROOT, env=environment,
                                      stdout=stream, stderr=subprocess.STDOUT,
                                      timeout=60).returncode
            except subprocess.TimeoutExpired:
                pass
    text = (out / "host.log").read_text() if (out / "host.log").exists() else ""
    checks = {line[5:] for line in text.splitlines() if line.startswith("PASS ")}
    stable = hashes == {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in INPUTS}
    good = build.returncode == 0 and code == 0 and stable and REQUIRED <= checks and "FAIL" not in text
    result = {"passed": good, "scope": "Actual nk60_v2 hardware library with injected MMIO/SPI NOR/DMA models; does not validate physical wiring, clock accuracy, signal integrity or USB enumeration",
              "build_command": command, "build_returncode": build.returncode,
              "returncode": code, "required": sorted(REQUIRED), "missing": sorted(REQUIRED - checks),
              "source_stable": stable, "source_sha256": hashes,
              "sanitizers": ["address", "undefined"]}
    (out / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))
    if not good:
        print((out / "build.log").read_text()[-5000:] if build.returncode else text[-5000:])
    print(out / "results.json")
    return 0 if good else 1


if __name__ == "__main__":
    raise SystemExit(main())
