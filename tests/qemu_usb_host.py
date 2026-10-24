#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Actual C USB stack -> stock QEMU DWC2.

The NOS C host stack runs as a sanitized host executable. qtest bridges its MMIO
and DMA to QEMU raspi3b. The tiny WFI payload is not NOS and validates no NOS boot
or physical timing. Existing guest regressions separately cover target execution.
"""
import hashlib
import json
import os
from pathlib import Path
import select
import shutil
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
EXPECTED = {
    "keyboard": "qemu_dwc2_actual_c_hid_boot_keyboard_leds_and_input",
    "keyboard_fs": "qemu_dwc2_actual_c_hid_boot_keyboard_leds_and_input",
    "mouse": "qemu_dwc2_actual_c_hid_boot_mouse_input",
    "storage": "qemu_dwc2_actual_c_enumeration_msc_bot_scsi_capacity_read_write_sync",
}


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    out = ROOT / "out/microkernel/qemu-usb-host" / str(time.time_ns())
    out.mkdir(parents=True)
    sources = [
        "tests/host_usb_qtest.c",
        "servers/drivers/framework/device.c",
        "servers/drivers/framework/peripheral.c",
        *[str(p.relative_to(ROOT)) for p in sorted((ROOT / "servers/usb/host").glob("*.c"))],
    ]
    inputs = [
        *sources,
        "tests/qemu_usb_host.py",
        *[str(p.relative_to(ROOT)) for p in sorted((ROOT / "include/nos/driver").glob("*.h"))],
    ]
    hashes = {name: digest(ROOT / name) for name in inputs}
    executable = out / "test"
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
        str(executable),
    ]
    with (out / "build.log").open("w") as log:
        build = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    qemu = shutil.which("qemu-system-aarch64")
    results = []
    idle = out / "idle.bin"
    idle.write_bytes(bytes.fromhex("7f2003d5ffffff17"))  # A64 WFI; B back to WFI.
    if build.returncode == 0 and qemu:
        for kind in EXPECTED:
            keyboard = kind in {"keyboard", "keyboard_fs"}
            record = {"device": kind, "passed": False}
            q = p = stream = sock = None
            try:
                with tempfile.TemporaryDirectory(prefix="nos-usb-") as directory:
                    qpath, mpath = str(Path(directory) / "qtest"), str(Path(directory) / "qmp")
                    # port=1 avoids QEMU's implicit hub insertion. No hub/TT claim.
                    args = [
                        qemu,
                        "-M",
                        "raspi3b",
                        "-display",
                        "none",
                        "-kernel",
                        str(idle),
                        "-S",
                        "-qtest",
                        f"unix:{qpath},server=on,wait=off",
                        "-qtest-log",
                        str(out / (kind + "-qtest.log")),
                        "-qmp",
                        f"unix:{mpath},server=on,wait=off",
                    ]
                    disk = out / "disk.raw"
                    if kind == "storage":
                        disk.write_bytes(
                            bytes((i * 7) & 255 for i in range(512)) + bytes(512 * 2047)
                        )
                        record["initial_disk_sha256"] = digest(disk)
                        args += [
                            "-drive",
                            f"if=none,id=usbdisk,file={disk},format=raw",
                            "-device",
                            "usb-storage,id=peripheral,drive=usbdisk,port=1",
                        ]
                    else:
                        args += [
                            "-device",
                            ("usb-kbd" if keyboard else "usb-mouse")
                            + ",id=peripheral,port=1"
                            + (",usb_version=1" if kind == "keyboard_fs" else ""),
                        ]
                    record["qemu_command"] = args
                    with (out / (kind + "-qemu.log")).open("w") as qlog, (
                        out / (kind + "-driver.log")
                    ).open("w") as dlog:
                        q = subprocess.Popen(args, stdout=qlog, stderr=subprocess.STDOUT)
                        deadline = time.monotonic() + 90
                        while not (Path(qpath).exists() and Path(mpath).exists()):
                            if q.poll() is not None or time.monotonic() > deadline:
                                raise RuntimeError("QEMU did not open qtest/QMP sockets")
                            time.sleep(0.01)
                        sock = socket.socket(socket.AF_UNIX)
                        sock.settimeout(5)
                        sock.connect(mpath)
                        stream = sock.makefile("rwb", buffering=0)
                        greeting = json.loads(stream.readline())
                        record["qmp_version"] = greeting["QMP"]["version"]

                        def qmp(name, arguments=None):
                            request = {"execute": name}
                            if arguments is not None:
                                request["arguments"] = arguments
                            stream.write((json.dumps(request) + "\n").encode())
                            while True:
                                line = stream.readline()
                                if not line:
                                    raise RuntimeError("QMP disconnected")
                                reply = json.loads(line)
                                if "error" in reply:
                                    raise RuntimeError(reply["error"])
                                if "return" in reply:
                                    return reply["return"]

                        qmp("qmp_capabilities")
                        qmp("cont")
                        env = dict(
                            os.environ,
                            ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
                            UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
                        )
                        p = subprocess.Popen(
                            [
                                str(executable),
                                qpath,
                                "keyboard" if keyboard else kind,
                                "2" if kind == "keyboard_fs" else "3",
                            ],
                            stdout=subprocess.PIPE,
                            stderr=dlog,
                            cwd=ROOT,
                            env=env,
                        )
                        pending = b""
                        lines = []
                        while True:
                            if time.monotonic() > deadline:
                                raise TimeoutError("USB driver integration timeout")
                            readable, _, _ = select.select([p.stdout], [], [], 1)
                            if not readable:
                                continue
                            chunk = os.read(p.stdout.fileno(), 8192)
                            if not chunk:
                                break
                            pending += chunk
                            while b"\n" in pending:
                                line, pending = pending.split(b"\n", 1)
                                line = line.decode()
                                lines.append(line)
                                dlog.write(line + "\n")
                                dlog.flush()
                                if line == "READY input":
                                    if keyboard:
                                        events = [
                                            {
                                                "type": "key",
                                                "data": {
                                                    "down": True,
                                                    "key": {"type": "qcode", "data": "a"},
                                                },
                                            }
                                        ]
                                    else:
                                        events = [
                                            {"type": "rel", "data": {"axis": "x", "value": 10}}
                                        ]
                                    qmp("input-send-event", {"events": events})
                                elif line == "READY detach":
                                    qmp("device_del", {"id": "peripheral"})
                        record["returncode"] = p.wait(timeout=5)
                        required = {
                            "PASS " + EXPECTED[kind],
                            "PASS qemu_dwc2_real_hot_unplug_invalidates_generation",
                        }
                        record["missing"] = sorted(required - set(lines))
                        record["passed"] = record["returncode"] == 0 and not record["missing"]
                        p.stdout.close()
                        qmp("quit")
                        q.wait(timeout=5)
                        q = None
                    if kind == "storage":
                        expected = bytes(0x3C ^ (i & 255) for i in range(512))
                        raw = disk.read_bytes()
                        record["disk_write_verified"] = raw[4 * 512 : 5 * 512] == expected
                        record["final_disk_sha256"] = digest(disk)
                        record["passed"] &= record["disk_write_verified"]
            except Exception as error:
                record["error"] = str(error)
                record["passed"] = False
            finally:
                if p is not None and p.poll() is None:
                    p.kill()
                    p.wait(timeout=5)
                if stream is not None:
                    stream.close()
                if sock is not None:
                    sock.close()
                if q is not None and q.poll() is None:
                    q.terminate()
                    q.wait(timeout=5)
            results.append(record)
            print(("PASS " if record["passed"] else "FAIL ") + "QEMU DWC2 " + kind, flush=True)
            if not record["passed"]:
                print(json.dumps(record), flush=True)
                print(
                    (out / (kind + "-driver.log")).read_text()[-3000:]
                    if (out / (kind + "-driver.log")).exists()
                    else ""
                )
    stable = hashes == {name: digest(ROOT / name) for name in inputs}
    good = (
        build.returncode == 0
        and len(results) == len(EXPECTED)
        and all(r["passed"] for r in results)
        and stable
    )
    result = {
        "passed": good,
        "scope": "Actual sanitized C host stack and DWC2 DMA MMIO against stock QEMU raspi3b root keyboard/mouse/storage; NOT a NOS Raspberry Pi boot or physical OTG/timing test",
        "build_returncode": build.returncode,
        "source_stable": stable,
        "source_sha256": hashes,
        "qemu_path": qemu,
        "qemu_sha256": digest(qemu) if qemu else None,
        "idle_sha256": digest(idle),
        "tests": results,
        "sanitizers": ["address", "undefined", "leak"],
    }
    (out / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    if build.returncode:
        print((out / "build.log").read_text())
    print(out / "results.json")
    return 0 if good else 1


if __name__ == "__main__":
    raise SystemExit(main())
