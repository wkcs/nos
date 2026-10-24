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
import tarfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("esp32_port", ROOT / "scripts/esp32s3.py")
port = importlib.util.module_from_spec(spec)
spec.loader.exec_module(port)
MARKERS = [
    "PASS esp32s3_ble_gatt_state_descriptors_subscriptions_mtu_queue_atomicity",
    "PASS esp32s3_usb_class_transport_hid_stale_and_backend_errors",
    "PASS esp32s3_usb_otg_auto_discharge_wrap_and_overcurrent",
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
    "PASS esp32s3_managed_devices",
    "PASS esp32s3_device_capacity_no_leak",
    "PASS esp32s3_sdk_ota_reject_abort",
    "PASS esp32s3_task_recycle_no_stack_leak",
    "PASS esp32s3_nos_idf_regression",
]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def emulator_probe(report):
    archive = port.SDK_ROOT / port.QEMU_DIRECTORY / port.QEMU_ARCHIVE
    assert digest(archive) == port.QEMU_SHA256, "Emulator download changed"
    binary = port.qemu_binary()
    with tarfile.open(archive) as package:
        member = next(
            m for m in package.getmembers() if m.name.endswith("/bin/" + port.QEMU_EXECUTABLE)
        )
        selected_from_archive = hashlib.sha256(
            package.extractfile(member).read()
        ).hexdigest() == digest(binary)
        if not port.os.environ.get("NOS_ESP_QEMU"):
            assert selected_from_archive, "Installed QEMU differs from the verified archive"
    command = [
        str(binary),
        "-M",
        port.TARGET,
        "-S",
        "-display",
        "none",
        "-serial",
        "null",
        "-monitor",
        "stdio",
    ]
    result = subprocess.run(
        command,
        input="info qtree\nquit\n",
        text=True,
        capture_output=True,
        env=port.environment(),
        timeout=15,
        check=True,
    )
    (report / "machine-qtree.log").write_text(result.stdout + result.stderr)
    return {
        "download_url": port.QEMU_URL,
        "archive_sha256": port.QEMU_SHA256,
        "binary_sha256": digest(binary),
        "machine": port.TARGET,
        "qtree_exit_code": 0,
        "selected_from_verified_archive": selected_from_archive,
        "usb_controller_model": "dwc2" in result.stdout.lower(),
        "rf_models": "not implemented by the pinned machine; no RF hardware claim",
    }


def audit(build, ecosystem=False, regression=True):
    manifest = json.loads((build / "nos-build.json").read_text())
    assert manifest["source"] == port.source_identity(), "Stale build source identity"
    assert manifest["target"] == port.TARGET
    config = json.loads((build / "config/sdkconfig.json").read_text())
    assert config["IDF_TARGET"] == port.TARGET
    assert config["FREERTOS_UNICORE"] and config["FREERTOS_HZ"] == 100
    assert not config.get("ESP_SYSTEM_HW_STACK_GUARD", False)
    assert manifest["regression"] == regression and manifest["profile"] == (
        "ecosystem" if ecosystem else "runtime"
    )
    assert manifest["elf_sha256"] == digest(
        build / f"nos_{port.TARGET}.elf"
    ), "ELF changed since build"
    commands = json.loads((build / "compile_commands.json").read_text())
    files = {Path(item["file"]).resolve() for item in commands}
    forbidden = [str(p) for p in files if p.name == "tasks.c"]
    assert not forbidden, f"Original FreeRTOS scheduler compiled: {forbidden}"
    required = [
        "kernel/sched/scheduler.c",
        "kernel/sched/task.c",
        "kernel/mm/memory.c",
        "kernel/ipc/ipc.c",
        "arch/esp_idf/platform.c",
        "ports/esp_idf/components/freertos/nos_tasks.c",
        "servers/drivers/framework/device.c",
        "servers/drivers/framework/service.c",
        "ports/esp_idf/components/nos_devices/devices.c",
        "ports/esp_idf/components/nos_devices/serial_buses.c",
        "ports/esp_idf/components/nos_devices/storage.c",
        "ports/esp_idf/components/nos_devices/console.c",
        "servers/bluetooth/gatt.c",
        "servers/usb/host/classes.c",
        "servers/usb/host/otg.c",
    ]
    assert all((ROOT / p).resolve() in files for p in required)
    selected_arch = "riscv" if port.TARGET == "esp32c3" else "xtensa"
    other_arch = "xtensa" if selected_arch == "riscv" else "riscv"
    for filename in ("port.c", "portasm.S"):
        assert (
            port.IDF / "components/freertos/FreeRTOS-Kernel/portable" / selected_arch / filename
        ).resolve() in files
    assert not any(f"/portable/{other_arch}/" in str(p) for p in files)
    elf = (build / f"nos_{port.TARGET}.elf").read_bytes()
    assert elf[:6] == b"\x7fELF\x01\x01"
    assert int.from_bytes(elf[18:20], "little") == (243 if selected_arch == "riscv" else 94)
    nm = (
        port.TOOLS
        / f"tools/{port.COMPILER_FAMILY}-elf"
        / port.XTENSA_VERSION
        / f"{port.COMPILER_FAMILY}-elf/bin/{port.COMPILER_PREFIX}-elf-nm"
    )
    symbols = subprocess.check_output([nm, build / f"nos_{port.TARGET}.elf"], text=True)
    names = {line.split()[-1] for line in symbols.splitlines() if line.split()}
    for name in ("select_task", "scheduler_tick", "nos_idf_syscall", "vTaskSwitchContext"):
        assert name in names, f"Missing NOS scheduler mechanism: {name}"
    assert not any(name.startswith("nos_sdk_unused_") for name in names)
    assert "vPortTCBPreDeleteHook" not in names, "SDK cleanup assumes a different TCB layout"
    application = list((ROOT / "ports/esp_idf/project/main").glob("*.c")) + [
        ROOT / "tests/guest/esp32s3_regression.c",
        ROOT / "tests/guest/esp32s3_protocols.c",
    ]
    subprocess.run(["python3", ROOT / "scripts/check_idf_app_devices.py", *application], check=True)
    mechanisms = ["nos_service_start", "nos_idf_devices_init"]
    if regression:
        mechanisms += ["nos_service_call", "nos_service_stop", "nos_idf_devices_snapshot"]
    for name in mechanisms:
        assert name in names, f"Missing managed device mechanism: {name}"
    if not regression:
        for name in (
            "nos_idf_shell_run",
            "nos_shell_editor_feed",
            "nos_shell_parse",
            "nos_idf_console_call",
        ):
            assert name in names, f"Missing interactive nshell mechanism: {name}"
        for path in (
            "ports/esp_idf/components/nos_shell/shell.c",
            "user/shell/editor.c",
            "user/shell/parser.c",
        ):
            assert (ROOT / path).resolve() in files
    checked = []
    c3 = port.TARGET == "esp32c3"
    if ecosystem:
        for path in (
            "auxiliary.c",
            "wireless.c",
            "ble.c",
            "usb_serial.c",
            "can.c",
        ):
            assert (ROOT / "ports/esp_idf/components/nos_devices" / path).resolve() in files
        for name in (
            "idf_auxiliary_ops",
            "idf_wifi_ops",
            "idf_ble_ops",
            "idf_usb_serial_ops",
            "idf_can_ops",
            "nos_usb_hid_open_transport",
        ):
            assert name in names, f"Missing managed SDK backend: {name}"
        usb_ops = ("idf_usb_host_ops", "idf_usb_device_ops", "idf_usb_otg_ops")
        if c3:
            assert all(name not in names for name in usb_ops), "C3 cannot own an OTG controller"
            absent = {"usb.c", "usb_host.c", "usb_otg.c"}
            assert not any(p.name in absent and "nos_devices" in str(p) for p in files)
        else:
            assert all(name in names for name in (*usb_ops, "nos_usb_msc_open_transport"))
        source = (ROOT / "ports/esp_idf/components/nos_devices/ecosystem.c").read_text()
        checked = [name for name in re.findall(r"\(uintptr_t\)(\w+)", source) if name in names]
        assert len(checked) == (26 if c3 else 35)
        sized_symbols = subprocess.check_output(
            [nm, "-S", build / f"nos_{port.TARGET}.elf"], text=True
        )
        table = next(
            line.split() for line in sized_symbols.splitlines() if line.endswith(" entry_points")
        )
        assert int(table[1], 16) == len(checked) * 4, "SDK entry table does not match the audit"
    return {
        "build_manifest": manifest,
        "nos_scheduler": True,
        "managed_device_model": True,
        "application_sdk_hardware_includes": False,
        "original_tasks_c_compiled": False,
        "retained_sdk_collections": ["list", "queue", "timers", "event_groups", "stream_buffer"],
        "sdk_entry_points": checked,
        "elf_sha256": digest(build / f"nos_{port.TARGET}.elf"),
        "image_sha256": digest(build / f"nos_{port.TARGET}.bin"),
    }


def run_qemu(build, report, timeout, markers=None, calibrated_efuse=False, interactive=False):
    report.mkdir(parents=True, exist_ok=True)
    markers = MARKERS if markers is None else markers
    markers = [
        m.replace("esp32s3", port.TARGET).replace(
            "_fpu_", "_fp_math_" if port.TARGET == "esp32c3" else "_fpu_"
        )
        for m in markers
    ]
    qemu = port.qemu_binary()
    env = port.environment()
    version = subprocess.check_output([qemu, "--version"], text=True, env=env).splitlines()[0]
    machines = subprocess.check_output([qemu, "-machine", "help"], text=True, env=env)
    assert re.search(
        r"^" + port.TARGET + r"\s", machines, re.M
    ), "QEMU must model the selected ESP32 chip"
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
    if calibrated_efuse:
        # S3 QEMU stores BLK0/1 as 24 bytes each. BLK2 bit128 is the
        # factory ADC-calibration version (1). Zero deltas select SDK-defined
        # base codes. This models fused calibration, not ADC conversion.
        efuse[64] = 1
        if port.TARGET == "esp32c3":
            efuse[39] = 3  # Official C3 model default calibration revision 1.3.
    efuse_path = report / "efuse.bin"
    efuse_path.write_bytes(efuse)
    command = [
        str(qemu),
        "-M",
        port.TARGET,
        "-nographic",
        "-monitor",
        "none",
        "-drive",
        f"file={flash_path},if=mtd,format=raw",
        "-drive",
        f"file={efuse_path},if=none,format=raw,id=efuse",
        "-global",
        f"driver=nvram.{port.TARGET}.efuse,property=drive,value=efuse",
    ]
    log = report / "qemu.log"
    deadline = time.monotonic() + timeout
    completed = False
    exchanges = []
    if interactive:
        arch = "riscv32-esp32c3" if port.TARGET == "esp32c3" else "xtensa-esp32s3"
        label = "C3" if port.TARGET == "esp32c3" else "S3"
        exchanges = [
            ("quoted_echo", f'echo "{label}" "console" "ok"\r'.encode(), rf"{label} console ok"),
            ("version", b"version\r", rf"NOS arch={arch} abi=1 target={port.TARGET} scheduler=NOS"),
            (
                "help",
                b"help\r",
                r"nshell commands: help version uptime stats mem ps devices echo history clear",
            ),
            (
                "native_stats",
                b"stats\r",
                r"tasks=[1-9][0-9]* mutexes=[0-9]+ endpoints=[0-9]+ free_pages=[1-9][0-9]*",
            ),
            (
                "native_tasks",
                b"ps\r",
                r"id=[0-9]+ name=main priority=[0-9]+ state=[0-9]+ stack_free=[0-9]+",
            ),
            (
                "managed_console",
                b"devices\r",
                r"system\.console kind=[0-9]+ state=running owned=[1-9][0-9]*",
            ),
            ("editing", b'echo "edit" baa\x7fr\r', r"edit bar"),
            ("history_recall", b"\x1b[A\r", r"edit bar"),
            ("history_list", b"history\r", r'echo "edit" bar'),
            ("quote_error", b'echo "unterminated\r', r"parse: unmatched quote"),
            ("line_overflow", b"echo " + b"x" * 300 + b"\r", r"command too long"),
            ("cancel", b'echo "must not execute"\x03', r'echo "must not execute"\^C'),
            ("unknown_command", b"__not_a_command\r", r"nshell: unknown command: __not_a_command"),
            ("vfs_boundary", b"ls\r", r"ls: unsupported; NOS VFS/ELF is not bound on this profile"),
            ("recovery", b'echo "shell" "still" "alive"\r', r"shell still alive"),
        ]
    exchange_index = 0
    exchange_start = None
    checks = []
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
                if interactive:
                    if "Returned from app_main()" in text:
                        break  # The normal application must keep the console alive.
                    ready = all(marker in text for marker in markers) and text.endswith("nos> ")
                    if exchange_start is None and ready:
                        exchange_start = len(text)
                        process.stdin.write(exchanges[exchange_index][1])
                        process.stdin.flush()
                    elif exchange_start is not None:
                        name, data, expected = exchanges[exchange_index]
                        response = text[exchange_start:]
                        if re.search(
                            r"(?:^|\n)" + expected + r"\n", response
                        ) and response.endswith("nos> "):
                            assert not re.search(r"(?:^|\n)must not execute\n", response)
                            checks.append(
                                {
                                    "name": name,
                                    "input_hex": data.hex(),
                                    "expected": expected,
                                    "passed": True,
                                }
                            )
                            exchange_index += 1
                            if exchange_index == len(exchanges):
                                completed = True
                                break
                            exchange_start = len(text)
                            process.stdin.write(exchanges[exchange_index][1])
                            process.stdin.flush()
                elif (
                    all(marker in text for marker in markers) and "Returned from app_main()" in text
                ):
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
        "machine": port.TARGET,
        "version": version,
        "command": command,
        "markers": markers,
        "harness_stopped_after_markers": harness_stopped,
        "qemu_exit_code": process.returncode,
        "flash_sha256": digest(flash_path),
        "hardware_peripherals": "not verified",
        "wifi_ble_rf": "not verified",
        "factory_calibration_efuse": calibrated_efuse,
        "interactive_console": {
            "enabled": interactive,
            "transport": "managed UART0",
            "checks": checks,
        },
    }


def manager_smoke(report):
    """Exercise IDF 5.4's actual dependency-manager protocol in a scratch app."""
    env = port.environment()
    env["IDF_COMPONENT_MANAGER"] = "1"
    python = port.python_environment() / "bin/python"
    version = subprocess.check_output(
        [
            python,
            "-c",
            "import importlib.metadata; print(importlib.metadata.version('idf-component-manager'))",
        ],
        text=True,
        env=env,
    ).strip()
    assert version == "2.5.2", f"Unexpected SDK component manager: {version}"
    project = report / "manager-project"
    (project / "main").mkdir(parents=True)
    (project / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.16)\n"
        f'set(EXTRA_COMPONENT_DIRS "{ROOT}/ports/esp_idf/components")\n'
        "set(COMPONENTS main)\ninclude($ENV{IDF_PATH}/tools/cmake/project.cmake)\n"
        "project(nos_manager_probe)\n"
    )
    (project / "main/CMakeLists.txt").write_text(
        'idf_component_register(SRCS "main.c" REQUIRES freertos)\n'
    )
    (project / "main/main.c").write_text("void app_main(void) {}\n")
    (project / "main/idf_component.yml").write_text('dependencies:\n  idf: ">=5.4,<5.5"\n')
    (project / "partitions.csv").write_bytes(
        (ROOT / "ports/esp_idf/project/partitions.csv").read_bytes()
    )
    build = report / "manager-build"
    with (report / "manager-configure.log").open("wb") as log:
        subprocess.run(
            [
                python,
                port.IDF / "tools/idf.py",
                "-C",
                project,
                "-B",
                build,
                f"-DIDF_TARGET={port.TARGET}",
                f"-DSDKCONFIG={report}/manager-sdkconfig",
                "-DSDKCONFIG_DEFAULTS=" + ";".join(map(str, port.sdkconfig_defaults())),
                "reconfigure",
            ],
            env=env,
            cwd=ROOT,
            stdout=log,
            stderr=subprocess.STDOUT,
            check=True,
        )
    commands = json.loads((build / "compile_commands.json").read_text())
    assert not any(Path(c["file"]).name == "tasks.c" for c in commands)
    assert any(Path(c["file"]).name == "nos_tasks.c" for c in commands)
    ulp = subprocess.check_output(
        ["esp32ulp-elf-as", "--version"], text=True, env=env
    ).splitlines()[0]
    return {
        "version": version,
        "idf_interface": 3,
        "cmake_exit_code": 0,
        "nos_component_override": True,
        "ulp_assembler": ulp,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output-root", type=Path, default=ROOT / "out" / port.TARGET / "validation"
    )
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
        result["emulator"] = emulator_probe(report)
        for name, script in (
            ("portable_protocols", "host_esp32s3_protocols.py"),
            ("usb_sdk_faults", "host_esp32s3_usb.py"),
        ):
            with (report / (name + ".log")).open("wb") as log:
                subprocess.run(
                    ["python3", ROOT / "tests" / script],
                    cwd=ROOT,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    check=True,
                )
            result[name] = {
                "passed": True,
                "scope": "actual code with transport/SDK model, no physical RF/USB claim",
            }
        subprocess.run(
            [
                "python3",
                ROOT / "tests/host_esp32s3_wireless.py",
                "--output",
                report / "wifi-faults",
            ],
            cwd=ROOT,
            check=True,
        )
        result["wifi_fault_model"] = json.loads((report / "wifi-faults/results.json").read_text())
        result["component_manager"] = manager_smoke(report)
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
                    ROOT / "scripts" / f"{port.TARGET}.py",
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
            result["qemu_ecosystem"] = run_qemu(
                output / "ecosystem/build",
                report / "ecosystem",
                args.timeout,
                [
                    *MARKERS,
                    "PASS esp32s3_sdk_ecosystem_linkage",
                    *(
                        ["PASS esp32s3_target_capability_limits"]
                        if port.TARGET == "esp32c3"
                        else ["PASS esp32s3_managed_otg_idle_worker"]
                    ),
                ],
                calibrated_efuse=True,
            )
        if not args.runtime_only:
            result["qemu_normal"] = run_qemu(
                output / "normal/build",
                report / "normal",
                args.timeout,
                [
                    "PASS page_allocator",
                    "NOS IDF application ready",
                    "[nos] nshell: starting",
                    "NOS SHELL READY",
                ],
                interactive=True,
            )
        result["source_after"] = port.source_identity()
        assert before == result["source_after"], "Sources changed during ESP32-S3 validation"
        result["passed"] = True
    finally:
        (report / "results.json").write_text(json.dumps(result, indent=2) + "\n")
        print(report / "results.json", flush=True)
    print(f"PASS {port.TARGET} NOS scheduler and SDK compatibility")


if __name__ == "__main__":
    main()
