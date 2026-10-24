#!/usr/bin/env python3
# Copyright (C) 2026 NOS contributors.
"""Same portable GATT/classes/OTG checks on a sanitized host and real S3 CPU."""
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    out = ROOT / "out/esp32s3/host-protocols" / str(time.time_ns())
    out.mkdir(parents=True)
    (out / "main.c").write_text(
        "void nos_esp32s3_protocol_regression(void); int main(void) { nos_esp32s3_protocol_regression(); }\n"
    )
    subprocess.run(
        [
            "gcc",
            "-Iinclude",
            "-Iinclude/nos",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            "-fno-pie",
            "-no-pie",
            "-g",
            str(out / "main.c"),
            "tests/guest/esp32s3_protocols.c",
            "servers/bluetooth/gatt.c",
            "servers/usb/host/classes.c",
            "servers/usb/host/core.c",
            "servers/usb/host/otg.c",
            "servers/drivers/framework/device.c",
            "servers/drivers/framework/peripheral.c",
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
