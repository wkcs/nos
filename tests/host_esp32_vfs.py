#!/usr/bin/env python3
# Copyright (C) 2026 NOS contributors.
"""Actual ESP VFS capacity/backends with mocked IPC, heap and sector I/O."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    output = ROOT / "out/esp32-vfs-model" / str(time.time_ns())
    output.mkdir(parents=True)
    results = []
    for chip in ("esp32c3", "esp32s3"):
        binary = output / chip
        command = [
            "cc",
            "-g",
            "-O1",
            "-DNOS_ESP_IDF",
            "-DNOS_ESP_VFS",
            "-Iinclude/nos",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            "-fno-pie",
            "-no-pie",
            "-ffunction-sections",
            "-fdata-sections",
            "-Wl,--gc-sections",
            "tests/host_esp32_vfs.c",
            "-o",
            str(binary),
        ]
        if chip == "esp32c3":
            command.append("-DNOS_IDF_RISCV")
        with (output / (chip + "-build.log")).open("wb") as log:
            build = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        code = None
        if not build.returncode:
            with (output / (chip + ".log")).open("wb") as log:
                result = subprocess.run(
                    [binary],
                    cwd=ROOT,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    env=dict(
                        os.environ,
                        ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
                        UBSAN_OPTIONS="halt_on_error=1",
                    ),
                    timeout=120,
                )
                code = result.returncode
        text = (output / (chip + ".log")).read_text() if code is not None else ""
        results.append(
            {
                "target": chip,
                "build_exit_code": build.returncode,
                "exit_code": code,
                "passed": code == 0 and text.count("PASS ") == 4,
            }
        )
    inputs = ["tests/host_esp32_vfs.c", "tests/host_esp32_vfs.py", "include/nos/abi.h"]
    inputs += [str(p.relative_to(ROOT)) for p in (ROOT / "servers/vfs").glob("*") if p.is_file()]
    report = {
        "passed": all(r["passed"] for r in results),
        "profiles": results,
        "inputs": {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in inputs},
        "sanitizers": ["address", "undefined", "leak"],
        "scope": "Actual VFS, mocked allocator/IPC/block boundary; not hardware or native IPC",
    }
    (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report["profiles"]), flush=True)
    print(output / "results.json", flush=True)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
