#!/usr/bin/env python3
# Copyright (C) 2026 NOS contributors.
"""Exercise actual SDK USB bridge against a callback/URB fault model."""
import os
import re
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    out = ROOT / "out/esp32s3/usb-host-model" / str(time.time_ns())
    out.mkdir(parents=True)
    source = (ROOT / "ports/esp_idf/components/nos_devices/usb_host.c").read_text()
    source = re.sub(r'^#include "[^\n]+"\n', "", source, flags=re.M)
    (out / "bridge.c").write_text('#include "host_esp32s3_usb_sdk.h"\n' + source)
    otg = (ROOT / "ports/esp_idf/components/nos_devices/usb_otg.c").read_text()
    otg = re.sub(r'^#include "[^\n]+"\n', "", otg, flags=re.M)
    (out / "otg.c").write_text('#include "host_esp32s3_usb_sdk.h"\n' + otg)
    subprocess.run(
        [
            "gcc",
            "-Iinclude",
            "-Iinclude/nos",
            "-Itests",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            "-fno-pie",
            "-no-pie",
            "-g",
            str(out / "bridge.c"),
            str(out / "otg.c"),
            "tests/host_esp32s3_usb.c",
            "servers/usb/host/otg.c",
            "servers/drivers/framework/device.c",
            "servers/drivers/framework/service.c",
            "servers/drivers/framework/peripheral.c",
            "servers/usb/host/classes.c",
            "servers/usb/host/core.c",
            "-o",
            str(out / "test"),
        ],
        cwd=ROOT,
        check=True,
    )
    subprocess.run(
        [out / "test"],
        cwd=ROOT,
        check=True,
        timeout=30,
        env=dict(
            os.environ,
            ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
            UBSAN_OPTIONS="halt_on_error=1",
        ),
    )


if __name__ == "__main__":
    main()
