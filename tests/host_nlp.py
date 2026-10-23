#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. NLP C sanitizers and independent Python/C interoperability.

Physical devices, electrical buses and the NOS-native nshell backend are not emulated.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import nlp_codec as wire
from nlp_crypto import Noise
from nlp_host import Client, Link
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey

REQUIRED = {
    "nlp_rpc_side_effect_once_under_response_backpressure_conflict_and_evicted_result",
    "nlp_30000_hostile_wire_tlv_utf8_uart_can_inputs_with_bounded_reassembly",
    "nlp_fabric_eight_level_real_router_admission_commit_directory_stale_lease_parent_loss_and_no_address_reuse",
    "nlp_boot_journal_torn_write_rollover_unique_boot_and_unknown_data_preserved",
    "nlp_keymap_sha256_generation_atomic_commit_idempotency_permission_and_disconnect",
    "nlp_display_tile_bounds_crc_atomic_present_owner_interval_and_disconnect_abort",
    "nlp_terminal_permission_real_bytes_interrupt_bounds_and_owner_disconnect_cleanup",
    "nlp_firmware_real_ed25519_sha256_manifest_resume_durable_offset_duplicate_write_apply_gate_and_idempotency",
    "nlp_radio_encrypted_authorization_bounded_async_job_secret_redaction_and_utf8_boundaries",
    "nlp_noise_xx_pinned_keys_aead_tamper_and_no_plaintext_downgrade",
    "nlp_reliable_credit_order_dedupe_identical_retry_backpressure_and_revocation",
    "nlp_stream_split_mailbox_stale_boot_token_and_can_classic_fd_actual_codec",
    "nlp_tree_prepare_commit_source_filter_expiry_and_input_source_release_lease_barrier",
}


def freeze():
    names = (
        subprocess.check_output(
            ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=ROOT
        )
        .decode()
        .split("\0")
    )
    return {
        name: (
            hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
            if (ROOT / name).is_file()
            else "MISSING"
        )
        for name in sorted(set(names))
        if name
    }


def independent_noise(log):
    a_key, b_key = bytes([33]) * 32, bytes([44]) * 32
    a_pub = X25519PrivateKey.from_private_bytes(a_key).public_key().public_bytes_raw()
    b_pub = X25519PrivateKey.from_private_bytes(b_key).public_key().public_bytes_raw()
    a = Noise(a_key, b_pub, True, b"nlp-test", bytes([1]) * 32)
    b = Noise(b_key, a_pub, False, b"nlp-test", bytes([2]) * 32)
    first = a.write()
    b.read(first)
    second = b.write()
    a.read(second)
    third = a.write()
    b.read(third)
    expected = dict(
        noise_msg1=first.hex(),
        noise_msg2=second.hex(),
        noise_msg3=third.hex(),
        noise_hash=a.hash.hex(),
        noise_tx=a.tx.hex(),
        noise_rx=a.rx.hex(),
    )
    actual = dict(line.split() for line in log.splitlines() if line.startswith("noise_"))
    assert actual == expected and a.tx == b.rx and a.rx == b.tx


def run(command, output, environment=None, timeout=60):
    result = subprocess.run(
        command, cwd=ROOT, env=environment, capture_output=True, text=True, timeout=timeout
    )
    output.write_text(result.stdout + result.stderr)
    return result


def free_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def network(server, directory, secure, client_seed, shell=False):
    server_key = X25519PrivateKey.from_private_bytes(bytes([33]) * 32)
    client_key = X25519PrivateKey.from_private_bytes(bytes([client_seed]) * 32)
    paths = {}
    for name, data in [
        ("server.key", server_key.private_bytes_raw()),
        ("client.key", client_key.private_bytes_raw()),
        ("server.pub", server_key.public_key().public_bytes_raw()),
        ("client.pub", client_key.public_key().public_bytes_raw()),
    ]:
        paths[name] = directory / name
        paths[name].write_bytes(data)
        paths[name].chmod(0o600)
    port = free_port()
    command = [str(server), "--secure-loopback" if secure else "--physical-loopback", str(port)]
    if secure:
        command += [str(paths["server.key"]), str(paths["client.pub"])]
        if shell:
            command += ["--shell"]
    stderr = (directory / "server.log").open("w")
    process = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=stderr, text=True)
    try:
        assert process.stdout.readline().startswith("NLP "), "server failed to start"
        base = [sys.executable, "scripts/nlp_host.py", "--tcp", f"127.0.0.1:{port}"]
        base += (
            ["--key", str(paths["client.key"]), "--peer-pin", str(paths["server.pub"])]
            if secure
            else ["--physical"]
        )
        for action in ["caps", "state", "monitor", "logs", "topology"]:
            result = run(base + [action, "--count", "1"], directory / f"{action}.log", timeout=15)
            assert result.returncode == 0, (action, result.stderr)
            if action == "caps":
                data = json.loads(result.stdout)
                assert len(data["capabilities"]) == (3 if shell else 2)
                assert data["security"] == (
                    "NOISE_XX_PINNED" if secure else "PHYSICALLY_TRUSTED_UNENCRYPTED"
                )
            elif action == "state":
                assert len(bytes.fromhex(result.stdout.strip())) == 52
            elif action in ("logs", "monitor"):
                assert json.loads(result.stdout)["opcode"] == 0x1001
            else:
                topology = json.loads(result.stdout)
                assert len(topology) == 2 and topology[1]["parent"] == topology[0]["node"]
        if secure:
            result = run(
                [
                    sys.executable,
                    "scripts/nlp_host.py",
                    "--tcp",
                    f"127.0.0.1:{port}",
                    "--physical",
                    "caps",
                ],
                directory / "downgrade.log",
                timeout=10,
            )
            assert result.returncode != 0 and "profile unavailable" in result.stderr
            paths["wrong.pub"] = directory / "wrong.pub"
            paths["wrong.pub"].write_bytes(bytes(32))
            result = run(
                [
                    sys.executable,
                    "scripts/nlp_host.py",
                    "--tcp",
                    f"127.0.0.1:{port}",
                    "--key",
                    str(paths["client.key"]),
                    "--peer-pin",
                    str(paths["wrong.pub"]),
                    "caps",
                ],
                directory / "wrong-pin.log",
                timeout=10,
            )
            assert result.returncode != 0 and "pinned public key" in result.stderr
            # A bad packet cannot advance session/RPC state; the following valid RPC must still work.
            args = argparse.Namespace(
                tcp=f"127.0.0.1:{port}", uart=None, usb=None, key=paths["client.key"]
            )
            link = Link(args)
            client = Client(
                link,
                key=client_key.private_bytes_raw(),
                peer_pin=server_key.public_key().public_bytes_raw(),
            ).connect()
            try:
                import struct

                session = client.manager
                client.wait(lambda: session.tx_limit > 0)
                from dataclasses import replace

                header = client._header(session, wire.REQUEST, 7, True)
                header = replace(header, reliable_seq=1)
                payload = struct.pack("<IHBB", 1, 0, 0, 0)
                link.write(wire.encode_frame(header, payload))
                header = replace(header, packet_counter=session.counter)
                session.counter += 1
                corrupt = bytearray(session.ciphers[1].encode(header, payload))
                corrupt[-1] ^= 1
                link.write(bytes(corrupt))
                assert len(client.call(session, 7)) >= 32
            finally:
                client.close()
                link.close()
            if shell:
                result = run(
                    base + ["shell", "--exec", "printf 'NLP_PTY_OK\\n'; exit 7"],
                    directory / "terminal.log",
                    timeout=15,
                )
                assert result.returncode == 7 and "NLP_PTY_OK" in result.stdout
    finally:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)
        stderr.close()
        server_log = (directory / "server.log").read_text()
        assert (
            "AddressSanitizer" not in server_log and "runtime error:" not in server_log
        ), server_log
        for name in ("server.key", "client.key"):
            paths[name].unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-root", type=Path)
    args = parser.parse_args()
    output = args.output_root or ROOT / "out/microkernel/host-nlp" / str(time.time_ns())
    output = output.resolve()
    output.mkdir(parents=True)
    before = freeze()
    environment = os.environ.copy()
    environment.update(
        ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
        UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
    )
    sources = sorted(str(p.relative_to(ROOT)) for p in (ROOT / "servers/nlp").glob("*/*.c"))
    vendor = sorted(
        str(p.relative_to(ROOT)) for p in (ROOT / "third_party/tinycrypt/lib/source").glob("*.c")
    )
    tests = sorted(str(p.relative_to(ROOT)) for p in (ROOT / "tests").glob("host_nlp*.c"))
    flags = [
        os.environ.get("CC", "gcc"),
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
        "-Iinclude/nos",
        "-Ithird_party/tinycrypt/lib/include",
    ]
    report = {
        "scope": "actual C protocol/capabilities with modeled storage, panel and radio; independent Python/C loopback crypto; optional real host PTY; NO electrical/hardware or NOS-native nshell validation",
        "commands": [],
        "checks": [],
        "passed": False,
    }
    try:
        # Key provisioning must not overwrite a prior identity or expose private bytes.
        private, public = output / "generated.key", output / "generated.pub"
        command = [
            sys.executable,
            "scripts/nlp_keygen.py",
            "--private",
            str(private),
            "--public",
            str(public),
        ]
        generated = run(command, output / "keygen.log")
        assert generated.returncode == 0
        original = private.read_bytes()
        assert private.stat().st_mode & 0o777 == 0o600 and len(original) == 32
        assert (
            X25519PrivateKey.from_private_bytes(original).public_key().public_bytes_raw()
            == public.read_bytes()
        )
        assert original.hex() not in generated.stdout
        assert run(command, output / "keygen-refuse-overwrite.log").returncode != 0
        assert private.read_bytes() == original
        private.unlink()
        public.unlink()
        report["checks"].append(
            "keygen_entropy_private_permissions_public_binding_and_no_overwrite"
        )
        for maximum in (128, 1024):
            directory = output / str(maximum)
            directory.mkdir()
            binary = directory / "test"
            command = flags + [
                f"-DNLP_PAYLOAD_MAX={maximum}",
                *tests,
                "apps/nlp-host/crypto_openssl.c",
                *sources,
                *vendor,
                "-lcrypto",
                "-o",
                str(binary),
            ]
            report["commands"].append(command)
            build = run(command, directory / "build.log")
            assert build.returncode == 0, build.stderr
            result = run([str(binary)], directory / "test.log", environment)
            assert result.returncode == 0, result.stdout + result.stderr
            markers = {line[5:] for line in result.stdout.splitlines() if line.startswith("PASS ")}
            assert REQUIRED <= markers and "NLP_C_CORE_OK" in result.stdout
            independent_noise(result.stdout)
            vectors = json.loads((ROOT / "protocols/nlp/v1/vectors.json").read_text())
            for vector in vectors["frames"]:
                result = run(
                    [str(binary), "wire", vector["pdu_hex"]],
                    directory / f'vector-{vector["name"]}.log',
                    environment,
                )
                assert (
                    result.returncode == 0 and result.stdout.strip() == "uart " + vector["uart_hex"]
                )
            rng = random.Random(20261023)
            for size in (0, 3, 55, 56, 63, 64, 65, 127, 128, 129, 1024, 1056):
                data = rng.randbytes(size)
                result = run(
                    [str(binary), "sha256", data.hex()], directory / f"sha-{size}.log", environment
                )
                assert (
                    result.returncode == 0
                    and result.stdout.strip() == "sha256 " + hashlib.sha256(data).hexdigest()
                )
            report["checks"].append(
                f"C_{maximum}_13_groups_30000_fuzz_7_wire_vectors_SHA256_and_independent_Noise"
            )
            print("PASS " + report["checks"][-1], flush=True)
        server = output / "nlp-server"
        command = flags + [
            *sorted(str(p.relative_to(ROOT)) for p in (ROOT / "apps/nlp-host").glob("*.c")),
            *sources,
            *vendor,
            "-lcrypto",
            "-lutil",
            "-o",
            str(server),
        ]
        report["commands"].append(command)
        build = run(command, output / "server-build.log")
        assert build.returncode == 0, build.stderr
        server_uid = hashlib.sha256(
            X25519PrivateKey.from_private_bytes(bytes([33]) * 32).public_key().public_bytes_raw()
        ).digest()[:16]
        seeds = []
        for lower in (True, False):
            seed = next(
                i
                for i in range(1, 255)
                if i != 33
                and (
                    hashlib.sha256(
                        X25519PrivateKey.from_private_bytes(bytes([i]) * 32)
                        .public_key()
                        .public_bytes_raw()
                    ).digest()[:16]
                    < server_uid
                )
                == lower
            )
            seeds.append(seed)
        for name, secure, seed, shell in [
            ("physical", False, seeds[0], False),
            ("noise-client-initiator", True, seeds[0], True),
            ("noise-server-initiator", True, seeds[1], False),
        ]:
            directory = output / name
            directory.mkdir()
            network(server, directory, secure, seed, shell)
            report["checks"].append(
                name + "_discovery_caps_state_subscription_logs_topology_and_authorization"
            )
            print("PASS " + report["checks"][-1], flush=True)
        report["passed"] = True
    except Exception as error:
        report["error"] = f"{type(error).__name__}: {error}"
        print("FAIL " + report["error"], flush=True)
    after = freeze()
    report["source_sha256"] = before
    report["source_stable"] = before == after
    report["passed"] = report["passed"] and report["source_stable"]
    report["exit_status"] = 0 if report["passed"] else 1
    (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(output / "results.json", flush=True)
    return report["exit_status"]


if __name__ == "__main__":
    sys.exit(main())
