#!/usr/bin/env python3
# Copyright (C) 2026 NOS contributors.
"""Real C3/S3 firmware upgrades and snapshot v3 persistence on Espressif QEMU."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import time
import traceback

import esp32s3 as suite

ROOT = Path(__file__).resolve().parents[1]
port = suite.port


def digest(data):
    return hashlib.sha256(data).hexdigest()


def partition(build, storage):
    table = (build / "partition_table/partition-table.bin").read_bytes()
    found = []
    for offset in range(0, len(table), 32):
        magic, kind, subtype, address, size, label, flags = struct.unpack_from(
            "<HBBII16sI", table, offset
        )
        if magic != 0x50AA:
            break
        if label.rstrip(b"\0") == b"storage":
            assert kind == 1 and subtype == (0x41 if storage == "wl" else 0x40) and not flags
            found.append((address, size))
    assert len(found) == 1
    return found[0]


def upgrade(build, original, destination, address, size):
    """Replace executable partitions deliberately; preserve storage byte for byte."""
    image = bytearray(b"\xff" * (4 * 1024 * 1024))
    for offset, filename in json.loads((build / "flasher_args.json").read_text())[
        "flash_files"
    ].items():
        offset = int(offset, 0)
        data = (build / filename).read_bytes()
        assert offset + len(data) <= address or offset >= address + size
        image[offset : offset + len(data)] = data
    previous = original.read_bytes()
    image[address : address + size] = previous[address : address + size]
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(image)
    return destination


def validate(build, legacy, report, storage, timeout):
    address, size = partition(build, storage)
    assert partition(legacy, storage) == (address, size)
    boots = {}
    checks = {}

    def boot(name, exchanges, image=None, firmware=build):
        result = suite.run_qemu(
            firmware,
            report / name,
            timeout,
            ["NOS: Newlib filesystem online", "NOS SHELL READY"],
            interactive=True,
            existing_flash=image,
            console_exchanges=exchanges,
        )
        assert result["qemu_exit_code"] == 0
        assert len(result["interactive_console"]["checks"]) == len(exchanges)
        boots[name] = result
        return report / name / "flash.bin" if image is None else image

    def content(image):
        return image.read_bytes()[address : address + size]

    def unchanged(name, before, image):
        after = content(image)
        assert before == after, name
        checks[name] = {"passed": True, "storage_sha256": digest(after)}

    boot(
        "normal",
        [("version", b"version\r", "filesystem=snapshot3"), ("console", b"echo alive\r", "alive")],
    )
    legacy_flash = boot(
        "legacy_seed",
        [
            ("mount", b"mount /data\r", "ok"),
            ("directory", b"mkdir /data/config\r", "ok"),
            ("write", b"write /data/config/saved stable-42\r", "ok"),
            ("commit1", b"sync\r", "ok"),
            ("append", b"append /data/config/saved -next\r", "ok"),
            ("commit2", b"sync\r", "ok"),
        ],
        firmware=legacy,
    )
    image = upgrade(build, legacy_flash, report / "upgrade/flash.bin", address, size)
    before = content(image)
    boot(
        "import_read_only",
        [
            ("mount", b"mount /data\r", "ok"),
            ("legacy", b"cat /data/config/saved\r", "stable-42-next"),
        ],
        image,
    )
    unchanged("import_does_not_write", before, image)
    first = "S" * 4096
    second = first + "T" * 4096
    boot(
        "migrate",
        [
            ("mount", b"mount /data\r", "ok"),
            *[
                (f"write_{i}", ("append /data/big " + "S" * 64 + "\r").encode(), "ok")
                for i in range(64)
            ],
            ("sync", b"sync\r", "ok"),
            ("read4096", b"cat /data/big\r", first),
        ],
        image,
    )
    migrated = image.read_bytes()
    downgraded = upgrade(legacy, image, report / "downgrade/flash.bin", address, size)
    before = content(downgraded)
    boot(
        "downgrade_rejected",
        [("mount", b"mount /data\r", "mount: error=-5"), ("alive", b"echo alive\r", "alive")],
        downgraded,
        legacy,
    )
    unchanged("old_firmware_cannot_mount_stale_v2", before, downgraded)
    image = upgrade(build, downgraded, report / "second/flash.bin", address, size)
    boot(
        "second_commit",
        [
            ("mount", b"mount /data\r", "ok"),
            ("read4096", b"cat /data/big\r", first),
            *[
                (f"append_{i}", ("append /data/big " + "T" * 64 + "\r").encode(), "ok")
                for i in range(64)
            ],
            ("sync", b"sync\r", "ok"),
            ("size8192", b"stat /data/big\r", r"size=8192 mode=100644"),
        ],
        image,
    )
    committed = image.read_bytes()
    before = content(image)
    boot(
        "limits_and_reboot",
        [
            ("mount", b"mount /data\r", "ok"),
            ("read8192", b"cat /data/big\r", second),
            ("file_limit", b"append /data/big x\r", "append: error=-28"),
            *[(f"touch_{i}", f"touch /data/empty{i}\r".encode(), "ok") for i in range(12)],
            ("record_limit", b"touch /data/overflow\r", "touch: error=-28"),
            ("alive", b"echo alive\r", "alive"),
        ],
        image,
    )
    unchanged("unsynced_limit_checks_do_not_write", before, image)

    if storage == "raw":
        stride = size // 4096 // 2
        header = bytes(b ^ 0xFF for b in committed[address : address + 32])
        h = struct.unpack("<8I", header)
        assert h[:3] == (0x4E4F5346, 3, 4) and h[4] == stride
        old = bytes(
            b ^ 0xFF for b in migrated[address + stride * 4096 : address + stride * 4096 + 32]
        )
        assert struct.unpack("<8I", old)[:3] == (0x4E4F5346, 3, 3)
        # Corrupt the final allocated data sector, well beyond the old 512-byte limit.
        last_data = 4 + h[5] - 1
        for name, sectors in (
            ("header_fallback", [0]),
            ("tail_fallback", [last_data]),
            ("both_banks_rejected", [0, stride]),
        ):
            fault = bytearray(committed)
            for sector in sectors:
                fault[address + sector * 4096] ^= 0x40
            target = report / name / "flash.bin"
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(fault)
            before = content(target)
            rejected = len(sectors) == 2
            boot(
                name,
                [
                    ("mount", b"mount /data\r", "mount: error=-5" if rejected else "ok"),
                    (
                        ("alive", b"echo alive\r", "alive")
                        if rejected
                        else ("older_data", b"cat /data/big\r", first)
                    ),
                ],
                target,
            )
            unchanged(name + "_read_only", before, target)
    else:
        # WL state copies are physical sectors N-3 and N-2; physical filesystem
        # locations rotate. Logical CRC recovery is also covered by the model.
        for name, offsets in (
            ("one_wl_copy_repaired", [size - 3 * 4096]),
            ("both_wl_copies_rejected", [size - 3 * 4096, size - 2 * 4096]),
        ):
            fault = bytearray(committed)
            for offset in offsets:
                fault[address + offset + 60] ^= 0x40
            target = report / name / "flash.bin"
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(fault)
            before = content(target)
            rejected = len(offsets) == 2
            boot(
                name,
                [
                    ("mount", b"mount /data\r", "mount: error=-5" if rejected else "ok"),
                    (
                        ("alive", b"echo alive\r", "alive")
                        if rejected
                        else ("data8192", b"cat /data/big\r", second)
                    ),
                ],
                target,
            )
            if rejected:
                unchanged(name + "_read_only", before, target)
        fault = bytearray(committed)
        fault[address : address + size] = b"\xff" * size
        fault[address + size // 2] = 0x5A
        target = report / "unknown/flash.bin"
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(fault)
        before = content(target)
        boot(
            "unknown_rejected",
            [("mount", b"mount /data\r", "mount: error=-5"), ("alive", b"echo alive\r", "alive")],
            target,
        )
        unchanged("unknown_rejected_read_only", before, target)
    return {
        "boots": boots,
        "checks": checks,
        "boot_count": len(boots),
        "storage_address": address,
        "storage_bytes": size,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=("esp32c3", "esp32s3"), required=True)
    parser.add_argument("--storage", choices=("raw", "wl"), default="raw")
    parser.add_argument("--output-root", type=Path)
    parser.add_argument("--legacy-build", type=Path)
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    if args.target == "esp32c3":
        port.configure_target(args.target)
    output = (
        args.output_root or ROOT / "out" / args.target / ("snapshot3-" + args.storage)
    ).resolve()
    report = output / "reports" / str(time.time_ns())
    report.mkdir(parents=True)
    source = port.source_identity()
    result = {
        "passed": False,
        "source": source,
        "target": args.target,
        "storage": args.storage,
        "filesystem": "snapshot3",
    }
    try:
        build = output / "normal/build"
        legacy = (args.legacy_build or output / "legacy/build").resolve()
        if not args.no_build:
            port.build(output=build.parent, storage=args.storage, filesystem="snapshot3")
            if not args.legacy_build:
                port.build(output=legacy.parent, storage=args.storage)
        result["image"] = suite.audit(
            build, regression=False, storage=args.storage, filesystem="snapshot3"
        )
        result["legacy_image"] = suite.audit(legacy, regression=False, storage=args.storage)
        result["persistence"] = validate(build, legacy, report, args.storage, args.timeout)
        assert source == port.source_identity(), "Inputs changed during validation"
        result["passed"] = True
    except Exception:
        result["error"] = traceback.format_exc()
    finally:
        result["source_after"] = port.source_identity()
        result["source_unchanged"] = source == result["source_after"]
        result["passed"] &= result["source_unchanged"]
        (report / "results.json").write_text(json.dumps(result, indent=2) + "\n")
        print(report / "results.json", flush=True)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
