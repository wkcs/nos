#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Sanitize actual keyboard service glue."""
import glob
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
INPUTS = [
    "tests/host_nk60_service.py",
    "tests/host_nk60_link.py",
    "scripts/nlp_host.py",
    "scripts/nlp_keyboard.py",
    "scripts/nlp_lighting.py",
    "scripts/nlp_codec.py",
    "scripts/nlp_crypto.py",
    "protocols/nlp/v1/registry.json",
    "tests/host_nk60_service.c",
    "servers/keyboard/service.c",
    "servers/keyboard/service.h",
    "servers/keyboard/keyboard.c",
    "servers/keyboard/keyboard.h",
    "servers/keyboard/keycodes.h",
    "servers/keyboard/keymap.c",
    "servers/keyboard/config.c",
    "servers/keyboard/config.h",
    "servers/usb/keyboard_usb.c",
    "servers/usb/keyboard_usb.h",
    "boards/nk60_v2/hardware.h",
    "boards/nk60_v2/nk60_usb.h",
]
REQUIRED = {
    "nk60_nlp_lighting_program_atomic_upload_behavior_render_save_validation_and_disconnect",
    "nk60_nlp_product_layout_storage_diagnostics_and_atomic_per_key_lighting_owner_timeout",
    "nk60_preferences_reboot_combined_save_unknown_schema_preserved_and_explicit_defaults",
    "nk60_nlp_real_service_discovery_keymap_sha_commit_and_peer_bound_unlock",
    "nk60_service_real_engine_usb_press_release",
    "nk60_service_usb_backpressure_retains_release",
    "nk60_service_physical_unlock_parser_and_nonblocking_uart",
    "nk60_service_flash_progress_keeps_usb_and_scan_alive",
    "nk60_service_suspend_rgb_budget_and_microsecond_wrap",
    "nk60_service_uart_macro_configuration_executes",
}


def main():
    out = ROOT / "out/microkernel/host-nk60-service" / str(time.time_ns())
    out.mkdir(parents=True)
    INPUTS.extend(
        str(p.relative_to(ROOT)) for p in sorted((ROOT / "examples/nk60/lighting").glob("*.json"))
    )
    INPUTS.extend(
        sorted(
            glob.glob("servers/nlp/*/*.c")
            + glob.glob("include/nos/nlp/*.h")
            + [
                "servers/keyboard/link.c",
                "servers/keyboard/link_caps.c",
                "servers/keyboard/link_fx.c",
                "servers/keyboard/link.h",
                "servers/keyboard/lighting.c",
                "servers/keyboard/lighting_fx.c",
                "servers/keyboard/lighting.h",
                "servers/keyboard/lighting_fx.h",
                "servers/keyboard/layout.c",
                "servers/keyboard/layout.h",
                "servers/keyboard/device.c",
            ]
            + glob.glob("third_party/tinycrypt/lib/source/*.c")
            + glob.glob("third_party/tinycrypt/lib/include/tinycrypt/*.h")
        )
    )
    INPUTS[:] = list(dict.fromkeys(INPUTS))
    hashes = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in INPUTS}
    command = [
        "gcc",
        "-Iinclude/nos",
        "-Ithird_party/tinycrypt/lib/include",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-std=gnu11",
        "-O1",
        "-g",
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
        "tests/host_nk60_service.c",
        "servers/keyboard/service.c",
        "servers/keyboard/keyboard.c",
        "servers/keyboard/keymap.c",
        "servers/keyboard/config.c",
        "servers/usb/keyboard_usb.c",
        "servers/keyboard/link.c",
        "servers/keyboard/link_caps.c",
        "servers/keyboard/link_fx.c",
        "servers/keyboard/lighting.c",
        "servers/keyboard/lighting_fx.c",
        "servers/keyboard/layout.c",
        "servers/keyboard/device.c",
        "third_party/tinycrypt/lib/source/sha256.c",
        "third_party/tinycrypt/lib/source/utils.c",
        *sorted(glob.glob("servers/nlp/*/*.c")),
        "-o",
        str(out / "host-nk60-service"),
    ]
    with (out / "build.log").open("w") as stream:
        build = subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT)
    code = None
    if not build.returncode:
        environment = os.environ.copy()
        environment.update(
            ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
            UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
        )
        with (out / "host.log").open("w") as stream:
            try:
                run = subprocess.run(
                    [str(out / "host-nk60-service")],
                    cwd=ROOT,
                    env=environment,
                    stdout=stream,
                    stderr=subprocess.STDOUT,
                    timeout=60,
                )
                code = run.returncode
            except subprocess.TimeoutExpired:
                pass
    link_good = False
    if code == 0:
        try:
            from host_nk60_link import interoperability

            interoperability(out / "host-nk60-service", out, environment)
            link_good = True
        except Exception as error:
            print("FAIL NK60 CLI interop:", repr(error), flush=True)
    text = (out / "host.log").read_text() if (out / "host.log").exists() else ""
    checks = {line[5:] for line in text.splitlines() if line.startswith("PASS ")}
    stable = hashes == {
        name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in INPUTS
    }
    good = (
        link_good
        and build.returncode == 0
        and code == 0
        and stable
        and REQUIRED <= checks
        and "FAIL" not in text
    )
    result = {
        "passed": good,
        "scope": "Actual service, debounce/layer engine, NOR journal and HID core with modeled physical drivers and host transport; no electrical or USB enumeration claim",
        "build_command": command,
        "build_returncode": build.returncode,
        "returncode": code,
        "independent_cli_interoperability": link_good,
        "required": sorted(REQUIRED),
        "missing": sorted(REQUIRED - checks),
        "source_stable": stable,
        "source_sha256": hashes,
        "sanitizers": ["address", "undefined"],
        "leak_detection": True,
    }
    (out / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))
    if not good:
        print((out / "build.log").read_text()[-5000:] if build.returncode else text[-5000:])
    print(out / "results.json")
    return 0 if good else 1


if __name__ == "__main__":
    raise SystemExit(main())
