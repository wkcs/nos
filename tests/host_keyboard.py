#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Exercise the real bounded keyboard engine."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import sys

ROOT = Path(__file__).resolve().parents[1]
INPUTS = [
    "tests/host_keyboard.c",
    "tests/host_keyboard_features.c",
    "tests/host_keyboard_lighting_fx.c",
    "tests/host_keyboard.py",
] + [
    "servers/keyboard/" + name
    for name in [
        "lighting.c",
        "lighting_fx.c",
        "lighting_fx.h",
        "lighting.h",
        "layout.c",
        "layout.h",
        "keyboard.c",
        "keyboard.h",
        "keymap.c",
        "keycodes.h",
        "config.c",
        "config.h",
    ]
]
INPUTS += ["scripts/nlp_lighting.py"] + [
    str(p.relative_to(ROOT)) for p in sorted((ROOT / "examples/nk60/lighting").glob("*.json"))
]
MARKERS = [
    "lighting_fx_macro_restart_rejection_preserves_real_start_and_end_time",
    "lighting_fx_newest_overlay_order_and_expired_instance_reclamation",
    "lighting_fx_required_excluded_layers_any_all_no_modifiers_origin_actions_and_bounded_wide_held_sources",
    "lighting_fx_actual_press_release_usage_tap_doubletap_overlap_and_expiry",
    "lighting_fx_modtap_tap_hold_interrupt_decision_and_configurable_held_threshold",
    "lighting_fx_layers_host_leds_modifiers_caps_word_macro_lifetime_and_reset",
    "lighting_fx_priority_blends_ripple_bounded_instances_saturation_and_global_power_limits",
    "lighting_fx_idle_wake_simultaneous_modifier_binding_preferences_migration_and_clock_wrap",
    "lighting_fx_50000_input_equivalence_and_10000_hostile_programs",
    "keyboard_ansi60_geometry_61_unique_per_key_leds",
    "keyboard_per_key_effects_current_limit_idle_suspend_codec_and_wrap",
    "keyboard_grave_escape_simultaneous_modifiers_caps_word_and_timeout",
    "keyboard_atomic_extended_preferences_legacy_preservation_and_every_byte_power_cut",
    "keyboard_symmetric_debounce_wrap",
    "keyboard_layer_binding_transparent_toggle",
    "keyboard_rollover_duplicate_modifier",
    "keyboard_tap_hold_oneshot",
    "keyboard_consumer_macro_backpressure_local",
    "keyboard_queue_fail_closed_atomic_remap",
    "keyboard_config_crc_dual_slot_fault_atomicity",
    "keyboard_stateful_fuzz_100000",
    "keyboard_all",
]


def main():
    out = ROOT / "out/microkernel/host-keyboard" / str(time.time_ns())
    out.mkdir(parents=True)
    command = [
        "gcc",
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
        "tests/host_keyboard.c",
        "tests/host_keyboard_features.c",
        "tests/host_keyboard_lighting_fx.c",
        "servers/keyboard/lighting.c",
        "servers/keyboard/lighting_fx.c",
        "servers/keyboard/layout.c",
        "servers/keyboard/keyboard.c",
        "servers/keyboard/keymap.c",
        "servers/keyboard/config.c",
        "-o",
        str(out / "keyboard"),
    ]
    with (out / "build.log").open("w") as log:
        build = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    code = None
    if build.returncode == 0:
        env = os.environ.copy()
        env.update(
            ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
            UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
        )
        with (out / "host.log").open("w") as log:
            try:
                code = subprocess.run(
                    [str(out / "keyboard")],
                    env=env,
                    cwd=ROOT,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    timeout=90,
                ).returncode
            except subprocess.TimeoutExpired:
                pass
    program_checks = False
    if code == 0:
        sys.path.insert(0, str(ROOT / "scripts"))
        from nlp_lighting import compile_program, decode_program

        try:
            examples = sorted((ROOT / "examples/nk60/lighting").glob("*.json"))
            assert len(examples) >= 3
            for example in examples:
                raw = compile_program(json.loads(example.read_text()))
                assert compile_program(decode_program(raw)) == raw
                result = subprocess.run(
                    [str(out / "keyboard"), "--fx-check", raw.hex()],
                    env=env,
                    capture_output=True,
                    text=True,
                    timeout=10,
                )
                assert result.returncode == 0 and result.stdout.strip() == "fx 0"
                for offset in (4, 5, 7, 8, 9, 12, 16, 17, 20, 21, 22, 46):
                    bad = bytearray(raw)
                    bad[offset] = 255
                    try:
                        decode_program(bytes(bad))
                        python_valid = True
                    except ValueError:
                        python_valid = False
                    result = subprocess.run(
                        [str(out / "keyboard"), "--fx-check", bad.hex()],
                        env=env,
                        capture_output=True,
                        text=True,
                        timeout=10,
                    )
                    assert result.returncode == 0 and (
                        (result.stdout.strip() == "fx 0") == python_valid
                    ), (example, offset, result.stdout)
            program_checks = True
            print(
                "PASS lighting_fx_independent_python_compiler_examples_and_c_validator_agree",
                flush=True,
            )
        except Exception as error:
            print("FAIL lighting program interoperability:", repr(error), flush=True)
    text = (out / "host.log").read_text() if (out / "host.log").exists() else ""
    result = {
        "passed": program_checks
        and build.returncode == 0
        and code == 0
        and "FAIL" not in text
        and all("PASS " + marker in text for marker in MARKERS),
        "scope": "real portable keyboard engine and fake NOR faults; no GPIO, USB electrical or real-board verification",
        "build_returncode": build.returncode,
        "returncode": code,
        "required_markers": MARKERS,
        "iterations": 100000,
        "fuzz_seed": "0x1badb002",
        "sanitizers": ["address", "undefined"],
        "compiler": subprocess.check_output(["gcc", "--version"], text=True).splitlines()[0],
        "source_sha256": {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in INPUTS},
    }
    (out / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(text if not build.returncode else (out / "build.log").read_text())
    print(out / "results.json")
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
