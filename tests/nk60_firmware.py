#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Cross-build real NK60 firmware, no board claim."""
import importlib.util
import json
from pathlib import Path
import subprocess
import shlex
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    spec = importlib.util.spec_from_file_location("flash_tool", ROOT / "scripts/flash_nk60_v2.py")
    tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tool)
    directory = ROOT / "out/microkernel/nk60-firmware" / str(time.time_ns())
    directory.mkdir(parents=True)
    results = []
    for mpu in (0, 1):
        output = directory / ("mpu" if mpu else "nompu")
        command = [
            "make",
            "PLATFORM=nk60_v2",
            "LIBC=minimal",
            "BUILD_MODE=normal",
            "SMP=1",
            f"MPU={mpu}",
            f"O={output.relative_to(ROOT)}",
            "firmware",
            "-j4",
        ]
        with (directory / f"build-{mpu}.log").open("w") as log:
            subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
        elf = output / "nos.elf"
        image = tool.validate_image(elf)
        raw = subprocess.check_output(["arm-none-eabi-nm", "-n", str(elf)], text=True)
        symbols = {}
        for line in raw.splitlines():
            fields = line.split()
            if len(fields) == 3:
                symbols[fields[2]] = int(fields[0], 16)
        assert "nos_vfs_server" not in symbols, "keyboard profile retained a VFS server"
        start, end = symbols["__user_text_start"], symbols["__user_text_end"]
        for function in (
            "nos_keyboard_service",
            "kb_scan",
            "nk60_matrix_scan",
            "nk60_usb_poll",
            "nlp_endpoint_receive",
            "nlp_session_receive",
            "nlp_frame_decode",
            "nlp_sha256",
            "tc_sha256_update",
            "kb_light_render",
            "kb_layout_position",
            "kb_config_save_extended",
            "nk60_link_device_register",
        ):
            assert start <= symbols[function] < end, f"{function} is outside user text"
        assert symbols["nk60_boot_init"] < start, "board clock init linked as a user driver"
        assert symbols["__user_bss_end"] + 4096 < symbols["__stack_top"] == 0x20010000
        assert symbols["__user_bss_end"] - symbols["__user_data_start"] <= 16384
        assert (output / "nos.bin").stat().st_size <= 128 * 1024
        assert (output / "nos.hex").stat().st_size > 0
        # Compile the same target inputs for static frame accounting, separately
        # from the firmware. These are known core chains, not measured high-water
        # marks or a proof covering every indirect hardware callback.
        config = json.loads((output / "build-config.json").read_text())
        stack_out = output / "stack-usage"
        stack_out.mkdir()
        for module in ("config", "service", "keyboard", "lighting"):
            subprocess.run(
                [
                    config["compiler"]["path"],
                    *shlex.split(config["configuration"]["cflags"]),
                    *shlex.split(config["configuration"]["user_flags"]),
                    "-fstack-usage",
                    "-c",
                    f"servers/keyboard/{module}.c",
                    "-o",
                    str(stack_out / (module + ".o")),
                ],
                cwd=ROOT,
                check=True,
                capture_output=True,
            )
        frames = {}
        for usage in stack_out.glob("*.su"):
            for line in usage.read_text().splitlines():
                location, amount, qualifier = line.split("\t")
                assert qualifier == "static", line
                frames[location.rsplit(":", 1)[1]] = int(amount)
        load_chain = sum(
            frames[name]
            for name in (
                "nos_keyboard_service",
                "nk60_service_init",
                "kb_config_load_extended",
                "kb_config_decode",
            )
        )
        save_core = sum(
            frames[name]
            for name in (
                "nos_keyboard_service",
                "nk60_service_step",
                "persist",
                "kb_config_save_extended",
                "save_record",
            )
        )
        assert load_chain <= 3072 and save_core <= 2560, "known core chain lost 4KiB stack reserve"
        results.append(
            {
                "mpu": bool(mpu),
                "image": str(elf.relative_to(ROOT)),
                **image,
                "static_ram_end": hex(symbols["__user_bss_end"]),
                "msp_reserve_bytes": symbols["__stack_top"] - symbols["__user_bss_end"],
                "bin_bytes": (output / "nos.bin").stat().st_size,
                "static_core_load_stack_bytes": load_chain,
                "static_core_save_stack_bytes": save_core,
                "stack_scope": "known core frames; excludes indirect driver callbacks and real high-water measurement",
                "user_static_bytes": symbols["__user_bss_end"] - symbols["__user_data_start"],
            }
        )
        print(
            f"PASS nk60_v2 cross-build MPU={mpu}: user service, 64 KiB SRAM / 128 KiB flash guards",
            flush=True,
        )
    (directory / "results.json").write_text(
        json.dumps({"passed": True, "board_tested": False, "builds": results}, indent=2) + "\n"
    )
    print("NK60 firmware results: " + str(directory / "results.json"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
