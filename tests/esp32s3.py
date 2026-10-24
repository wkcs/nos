#!/usr/bin/env python3
# Copyright (C) 2026 NOS contributors.
"""Run the actual ESP32-S3 image and audit the broad SDK linkage profile."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import time
import re

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("esp32_port", ROOT / "scripts/esp32s3.py")
port = importlib.util.module_from_spec(spec)
spec.loader.exec_module(port)
MARKERS = [
    "PASS page_allocator",
    "PASS esp32s3_native_syscalls",
    "PASS esp32s3_timer_preemption",
    "PASS esp32s3_queue_timeout",
    "PASS esp32s3_tls_errno_fpu_cleanup",
    "PASS esp32s3_event_stream",
    "PASS esp32s3_timers_notifications",
    "PASS esp32s3_native_ipc",
    "PASS esp32s3_priority_inheritance_native_exit",
    "PASS esp32s3_static_tasks_capacity_rollback",
    "PASS esp32s3_sdk_gpio_uart_flash_nvs",
    "PASS esp32s3_sdk_ota_reject_abort",
    "PASS esp32s3_task_recycle_no_stack_leak",
    "PASS esp32s3_nos_idf_regression",
]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def audit(build, ecosystem=False, regression=True):
    manifest = json.loads((build / "nos-build.json").read_text())
    assert manifest["source"] == port.source_identity(), "Stale build source identity"
    assert manifest["regression"] == regression and manifest["profile"] == (
        "ecosystem" if ecosystem else "runtime"
    )
    assert manifest["elf_sha256"] == digest(build / "nos_esp32s3.elf"), "ELF changed since build"
    commands = json.loads((build / "compile_commands.json").read_text())
    files = {Path(item["file"]).resolve() for item in commands}
    forbidden = [str(p) for p in files if p.name == "tasks.c"]
    assert not forbidden, f"Original FreeRTOS scheduler compiled: {forbidden}"
    required = [
        "kernel/sched/scheduler.c",
        "kernel/sched/task.c",
        "kernel/mm/memory.c",
        "kernel/ipc/ipc.c",
        "arch/xtensa/platform.c",
        "ports/esp_idf/components/freertos/nos_tasks.c",
    ]
    assert all((ROOT / p).resolve() in files for p in required)
    nm = (
        port.TOOLS
        / "tools/xtensa-esp-elf"
        / port.XTENSA_VERSION
        / "xtensa-esp-elf/bin/xtensa-esp32s3-elf-nm"
    )
    symbols = subprocess.check_output([nm, build / "nos_esp32s3.elf"], text=True)
    names = {line.split()[-1] for line in symbols.splitlines() if line.split()}
    for name in ("select_task", "scheduler_tick", "nos_xtensa_syscall", "vTaskSwitchContext"):
        assert name in names, f"Missing NOS scheduler mechanism: {name}"
    assert not any(name.startswith("nos_sdk_unused_") for name in names)
    assert "vPortTCBPreDeleteHook" not in names, "SDK cleanup assumes a different TCB layout"
    checked = []
    if ecosystem:
        source = (ROOT / "ports/esp_idf/project/main/ecosystem.c").read_text()
        checked = re.findall(r"\(uintptr_t\)(\w+)", source)
        assert len(checked) == 32 and all(name in names for name in checked)
    return {
        "build_manifest": manifest,
        "nos_scheduler": True,
        "original_tasks_c_compiled": False,
        "retained_sdk_collections": ["list", "queue", "timers", "event_groups", "stream_buffer"],
        "sdk_entry_points": checked,
        "elf_sha256": digest(build / "nos_esp32s3.elf"),
        "image_sha256": digest(build / "nos_esp32s3.bin"),
    }


def run_qemu(build, report, timeout, markers=None):
    report.mkdir(parents=True, exist_ok=True)
    markers = MARKERS if markers is None else markers
    qemu = port.qemu_binary()
    env = port.environment()
    version = subprocess.check_output([qemu, "--version"], text=True, env=env).splitlines()[0]
    machines = subprocess.check_output([qemu, "-machine", "help"], text=True, env=env)
    assert re.search(r"^esp32s3\s", machines, re.M), "QEMU must model the actual ESP32-S3"
    flash = bytearray(b"\xff" * (4 * 1024 * 1024))
    entries = json.loads((build / "flasher_args.json").read_text())["flash_files"]
    ranges = []
    for offset, filename in entries.items():
        offset = int(offset, 0)
        data = (build / filename).read_bytes()
        end = offset + len(data)
        assert end <= len(flash) and not any(offset < b and a < end for a, b in ranges)
        ranges.append((offset, end))
        flash[offset:end] = data
    flash_path = report / "flash.bin"
    flash_path.write_bytes(flash)
    # ESP-IDF 5.4.2's default QEMU eFuse image: chip revision 0.3, no keys.
    efuse = bytearray(1024)
    efuse[38] = 0x0C
    efuse_path = report / "efuse.bin"
    efuse_path.write_bytes(efuse)
    command = [
        str(qemu),
        "-M",
        "esp32s3",
        "-nographic",
        "-monitor",
        "none",
        "-drive",
        f"file={flash_path},if=mtd,format=raw",
        "-drive",
        f"file={efuse_path},if=none,format=raw,id=efuse",
        "-global",
        "driver=nvram.esp32s3.efuse,property=drive,value=efuse",
    ]
    log = report / "qemu.log"
    deadline = time.monotonic() + timeout
    completed = False
    with log.open("wb") as output:
        process = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=output,
            stderr=subprocess.STDOUT,
            cwd=ROOT,
            env=env,
        )
        try:
            # Keep stdin open: the QEMU UART drops TX when its backend sees EOF.
            while process.poll() is None and time.monotonic() < deadline:
                text = log.read_text(errors="replace").replace("\r", "")
                if "Guru Meditation" in text or "assert failed" in text or "FAIL " in text:
                    break
                if all(marker in text for marker in markers) and "Returned from app_main()" in text:
                    completed = True
                    break
                time.sleep(0.05)
        finally:
            harness_stopped = process.poll() is None
            if harness_stopped:
                process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            process.stdin.close()
    text = log.read_text(errors="replace").replace("\r", "")
    assert completed, f"ESP32-S3 regression failed; see {log}\n{text[-4000:]}"
    assert (
        harness_stopped and process.returncode == 0
    ), f"Unexpected QEMU exit: {process.returncode}"
    return {
        "machine": "esp32s3",
        "version": version,
        "command": command,
        "markers": markers,
        "harness_stopped_after_markers": harness_stopped,
        "qemu_exit_code": process.returncode,
        "flash_sha256": digest(flash_path),
        "hardware_peripherals": "not verified",
        "wifi_ble_rf": "not verified",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-root", type=Path, default=ROOT / "out/esp32s3/validation")
    parser.add_argument("--timeout", type=int, default=45)
    parser.add_argument("--runtime-only", action="store_true")
    parser.add_argument("--no-build", action="store_true")
    args = parser.parse_args()
    output = args.output_root.resolve()
    report = output / "reports" / str(time.time_ns())
    report.mkdir(parents=True)
    before = port.source_identity()
    result = {"source": before, "idf_commit": port.IDF_COMMIT, "passed": False}
    try:
        configurations = [("runtime", "runtime", True)]
        if not args.runtime_only:
            configurations = (
                [("normal", "runtime", False)] + configurations + [("ecosystem", "ecosystem", True)]
            )
        for name, profile, regression in configurations:
            build = output / name / "build"
            if not args.no_build:
                command = [
                    "python3",
                    ROOT / "scripts/esp32s3.py",
                    "build",
                    "--profile",
                    profile,
                    "--output",
                    output / name,
                ]
                if regression:
                    command.append("--regression")
                with (report / f"build-{name}.log").open("wb") as log:
                    subprocess.run(
                        command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True
                    )
            result[name] = audit(build, profile == "ecosystem", regression)
        result["qemu"] = run_qemu(output / "runtime/build", report / "runtime", args.timeout)
        if not args.runtime_only:
            result["qemu_normal"] = run_qemu(
                output / "normal/build",
                report / "normal",
                args.timeout,
                ["PASS page_allocator", "NOS IDF application ready"],
            )
        result["source_after"] = port.source_identity()
        assert before == result["source_after"], "Sources changed during ESP32-S3 validation"
        result["passed"] = True
    finally:
        (report / "results.json").write_text(json.dumps(result, indent=2) + "\n")
        print(report / "results.json", flush=True)
    print("PASS ESP32-S3 NOS scheduler and SDK compatibility")


if __name__ == "__main__":
    main()
