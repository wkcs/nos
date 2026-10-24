#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Independent Python CLI to actual C NK60 service.

The loopback C fixture models physical I/O and explicitly simulates the unlock
chord. This does not validate USB electrical behavior or operator presence.
"""
import json
from pathlib import Path
import socket
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def interoperability(binary, output, environment):
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    with (output / "fixture.log").open("w") as errors:
        server = subprocess.Popen(
            [str(binary), "--loopback", str(port)],
            cwd=ROOT,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=errors,
            text=True,
        )
        try:
            assert server.stdout.readline().startswith("NK60 NLP"), "fixture did not start"
            base = [
                sys.executable,
                "scripts/nlp_host.py",
                "--tcp",
                f"127.0.0.1:{port}",
                "--physical",
            ]
            counter = 0

            def call(action, *flags, success=True):
                nonlocal counter
                counter += 1
                result = subprocess.run(
                    [*base, action, *map(str, flags)],
                    cwd=ROOT,
                    capture_output=True,
                    text=True,
                    timeout=15,
                )
                (output / f"cli-{counter}-{action}.log").write_text(result.stdout + result.stderr)
                assert (result.returncode == 0) == success, (action, result.stdout, result.stderr)
                return json.loads(result.stdout) if success else result

            caps = call("caps")["capabilities"]
            assert len(caps) == 5
            info = call("info")
            assert (
                info["keys"] == info["leds"] == 61 and info["layers"] == 4 and info["width_u"] == 15
            )
            layout = call("layout")
            assert len(layout) == 61 and len({p["led"] for p in layout}) == 61
            storage = call("storage")
            assert storage["capacity"] == 16384 and len(storage["partitions"]) == 3
            assert call("health")["hard_rgb_channel_limit"] == 16
            call(
                "lighting-set",
                "--mode",
                "wave",
                "--brightness",
                220,
                "--rgb",
                "9,8,7",
                "--wait-unlock",
                10,
            )
            assert call("lighting")["mode"] == "wave"
            call(
                "lighting-policy",
                "--speed",
                90,
                "--idle-seconds",
                60,
                "--led-budget",
                150,
                "--save",
                "--wait-unlock",
                10,
            )
            policy = call("lighting")
            assert policy["speed"] == 90 and policy["estimated_led_budget_ma"] == 150
            colors = [[i, 255 - i, 17] for i in range(61)]
            incoming = output / "colors.json"
            incoming.write_text(json.dumps(colors))
            call("leds-put", "--file", incoming, "--save", "--wait-unlock", 10)
            actual = call("leds-get")["colors"]
            assert actual == colors
            assert call("lighting")["mode"] == "custom"
            storage = call("storage")
            assert storage["durable_generation"] >= 1 and not storage["dirty"]
            incoming.write_text(json.dumps([[256, 0, 0]] * 61))
            call("leds-put", "--file", incoming, "--wait-unlock", 10, success=False)
            assert call("leds-get")["colors"] == colors
            examples = ROOT / "examples/nk60/lighting"
            info = call("fx-info")
            assert info["max_rules"] == 12 and info["wire_bytes"] == 400
            for example in sorted(examples.glob("*.json")):
                call("fx-put", "--file", example, "--save", "--wait-unlock", 10)
                from importlib import import_module

                sys.path.insert(0, str(ROOT / "scripts"))
                compiler = import_module("nlp_lighting")
                expected = compiler.decode_program(
                    compiler.compile_program(json.loads(example.read_text()))
                )
                actual = call("fx-get")["program"]
                assert actual == expected
            previous = call("fx-get")["program"]
            invalid = output / "bad-program.json"
            invalid.write_text(json.dumps({"rules": [{"when": "press", "evil_opcode": 1}]}))
            call("fx-put", "--file", invalid, "--wait-unlock", 10, success=False)
            assert call("fx-get")["program"] == previous
            call("fx-clear", "--save", "--wait-unlock", 10)
            assert not call("fx-get")["program"]["enabled"]
            print(
                "PASS nk60_independent_python_json_effect_program_upload_readback_presets_and_atomic_rejection",
                flush=True,
            )
            print(
                "PASS nk60_independent_python_cli_actual_c_product_layout_storage_policy_atomic_leds_and_permissions",
                flush=True,
            )
        finally:
            server.terminate()
            try:
                server.wait(timeout=3)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=3)
    log = (output / "fixture.log").read_text()
    assert "AddressSanitizer" not in log and "runtime error:" not in log, log
