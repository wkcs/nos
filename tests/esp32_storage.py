#!/usr/bin/env python3
# Copyright (C) 2026 NOS contributors.
"""Independent QEMU boots, actual shared NOR image and corrupted snapshots."""
import hashlib
import json
from pathlib import Path
import struct


def persistence(run_qemu, build, output, timeout, storage="raw"):
    output.mkdir(parents=True, exist_ok=True)
    table = (build / "partition_table/partition-table.bin").read_bytes()
    partitions = []
    for offset in range(0, len(table), 32):
        magic, kind, subtype, address, size, label, flags = struct.unpack_from(
            "<HBBII16sI", table, offset
        )
        if magic != 0x50AA:
            break
        if label.rstrip(b"\0") == b"storage":
            assert kind == 1 and subtype == (0x41 if storage == "wl" else 0x40) and not flags
            partitions.append((address, size))
    assert len(partitions) == 1
    address, size = partitions[0]
    results = {}
    flash = output / "write/flash.bin"

    def boot(name, exchanges, image=None):
        result = run_qemu(
            build,
            output / name,
            timeout,
            ["NOS: Newlib filesystem online", "NOS SHELL READY"],
            interactive=True,
            existing_flash=image,
            console_exchanges=exchanges,
        )
        assert result["qemu_exit_code"] == 0
        assert len(result["interactive_console"]["checks"]) == len(exchanges)
        results[name] = result

    boot(
        "write",
        [
            ("mount", b"mount /data\r", r"ok"),
            ("directory", b"mkdir /data/config\r", r"ok"),
            ("write", b'write /data/config/note "stable-42"\r', r"ok"),
            ("commit", b"sync\r", r"ok"),
            ("volatile", b'write /tmp/volatile "RAM-only"\r', r"ok"),
        ],
    )
    boot(
        "remount",
        [
            ("mount", b"mount /data\r", r"ok"),
            ("persisted", b"cat /data/config/note\r", r"stable-42"),
            ("ram_lost", b"cat /tmp/volatile\r", r"cat: error=-2"),
            ("rename", b"mv /data/config/note /data/config/saved\r", r"ok"),
            ("append", b'append /data/config/saved "-next"\r', r"ok"),
            ("commit", b"sync\r", r"ok"),
        ],
        flash,
    )
    committed = flash.read_bytes()
    boot(
        "unsynced",
        [
            ("mount", b"mount /data\r", r"ok"),
            ("second_commit", b"cat /data/config/saved\r", r"stable-42-next"),
            ("rename_persisted", b"cat /data/config/note\r", r"cat: error=-2"),
            ("uncommitted_write", b'write /data/config/saved "uncommitted"\r', r"ok"),
        ],
        flash,
    )
    assert flash.read_bytes()[address : address + size] == committed[address : address + size]
    boot(
        "discard_unsynced",
        [
            ("mount", b"mount /data\r", r"ok"),
            ("old_commit", b"cat /data/config/saved\r", r"stable-42-next"),
        ],
        flash,
    )
    if storage == "raw":
        fallback = [
            ("mount_previous_bank", b"mount /data\r", r"ok"),
            ("first_commit", b"cat /data/config/note\r", r"stable-42"),
            ("newer_name_lost", b"cat /data/config/saved\r", r"cat: error=-2"),
        ]
        # Generation 1 is bank 1, generation 2 bank 0. Each logical sector owns
        # one 4K erase block. These faults affect the new bank exclusively.
        for name, relative in (("header_fallback", 20), ("payload_fallback", 4096 + 128)):
            image = output / (name + ".bin")
            damaged = bytearray(committed)
            damaged[address + relative] ^= 1
            image.write_bytes(damaged)
            boot(name, fallback, image)
        image = output / "both_bad.bin"
        damaged = bytearray(committed)
        for sector in (0, 33):
            damaged[address + sector * 4096 + 20] ^= 1
        image.write_bytes(damaged)
        boot(
            "corrupt_rejected",
            [
                ("reject_corruption", b"mount /data\r", r"mount: error=-5"),
                ("shell_survives", b'echo "storage" "error" "handled"\r', r"storage error handled"),
            ],
            image,
        )
        assert image.read_bytes()[address : address + size] == damaged[address : address + size]
        assert committed[address + 66 * 4096 : address + size] == b"\xff" * (size - 66 * 4096)
    else:
        # Pinned IDF 896KiB/4KiB layout: state copies at N-3 and N-2.
        assert size == 0xE0000
        image = output / "wl-one-state.bin"
        damaged = bytearray(committed)
        damaged[address + size - 3 * 4096 + 60] ^= 1
        image.write_bytes(damaged)
        boot(
            "wl_metadata_recovery",
            [
                ("mount_repair", b"mount /data\r", r"ok"),
                ("latest_commit", b"cat /data/config/saved\r", r"stable-42-next"),
            ],
            image,
        )
        damaged[address + size - 2 * 4096 + 60] ^= 1
        image = output / "wl-both-states.bin"
        image.write_bytes(damaged)
        boot(
            "corrupt_rejected",
            [
                ("no_reinitialize", b"mount /data\r", r"mount: error=-5"),
                ("shell_survives", b'echo "storage" "error" "handled"\r', r"storage error handled"),
            ],
            image,
        )
        assert image.read_bytes()[address : address + size] == damaged[address : address + size]
        damaged = bytearray(committed)
        damaged[address : address + size] = b"\xff" * size
        damaged[address + 42] = 0
        image = output / "wl-unknown.bin"
        image.write_bytes(damaged)
        boot(
            "unknown_rejected",
            [
                ("unknown_not_formatted", b"mount /data\r", r"mount: error=-5"),
                ("shell_survives", b'echo "unknown" "preserved"\r', r"unknown preserved"),
            ],
            image,
        )
        assert image.read_bytes()[address : address + size] == damaged[address : address + size]
    report = {
        "passed": True,
        "storage": storage,
        "boots": results,
        "boot_count": len(results),
        "storage_offset": address,
        "storage_bytes": size,
        "committed_flash_sha256": hashlib.sha256(committed).hexdigest(),
        "both_bad_storage_unchanged": True,
        "unused_storage_unchanged": storage == "raw",
        "scope": "actual QEMU NOR read/erase/write and independent boots; offline fault fixtures are not physical power-cut or flash-endurance tests",
    }
    (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    return report
