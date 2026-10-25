#!/usr/bin/env python3
# Copyright (C) 2026 NOS contributors.
"""Repeat the formerly failing C3 ecosystem / S3 runtime pair concurrently."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import importlib.util
import json
from pathlib import Path
import time

ROOT = Path(__file__).resolve().parents[1]


def suite(chip):
    spec = importlib.util.spec_from_file_location("timing_" + chip, ROOT / "tests/esp32s3.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    if chip == "esp32c3":
        module.port.configure_target(chip)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--c3-build", required=True, type=Path)
    parser.add_argument("--s3-build", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--rounds", type=int, default=4)
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    assert 1 <= args.rounds <= 32
    output = (args.output or ROOT / "out/esp32-timing-repeat" / str(time.time_ns())).resolve()
    output.mkdir(parents=True, exist_ok=False)
    modules = {chip: suite(chip) for chip in ("esp32c3", "esp32s3")}
    builds = {"esp32c3": args.c3_build.resolve(), "esp32s3": args.s3_build.resolve()}
    before = modules["esp32c3"].port.source_identity()
    assert not before["status"], "Freeze sources before timing validation"
    audits = {
        chip: module.audit(builds[chip], chip == "esp32c3", True, "raw")
        for chip, module in modules.items()
    }
    results = {"passed": False, "source": before, "audits": audits, "rounds": []}

    def run(chip, round_number):
        module = modules[chip]
        markers = list(module.MARKERS)
        if chip == "esp32c3":
            markers += [
                "PASS esp32s3_sdk_ecosystem_linkage",
                "PASS esp32s3_target_capability_limits",
            ]
        return module.run_qemu(
            builds[chip],
            output / f"round-{round_number}" / chip,
            args.timeout,
            markers,
            calibrated_efuse=chip == "esp32c3",
        )

    try:
        with ThreadPoolExecutor(max_workers=2) as pool:
            for round_number in range(args.rounds):
                futures = {chip: pool.submit(run, chip, round_number) for chip in modules}
                record = {}
                for chip, future in futures.items():
                    try:
                        record[chip] = future.result()
                    except Exception as error:
                        record[chip] = {"failed": True, "error": str(error)}
                results["rounds"].append(record)
                assert not any(x.get("failed", False) for x in record.values()), record
                for chip, module in modules.items():
                    assert (
                        module.digest(builds[chip] / f"nos_{chip}.elf")
                        == audits[chip]["elf_sha256"]
                    )
                print(f"PASS timing parallel round {round_number + 1}/{args.rounds}", flush=True)
        results["source_after"] = modules["esp32c3"].port.source_identity()
        assert results["source_after"] == before
        results["passed"] = True
    finally:
        (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        print(output / "results.json", flush=True)


if __name__ == "__main__":
    main()
