#!/usr/bin/env python3
"""运行 ARMv7-M MPU 平台参数化主机回归测试。"""

from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
CC = "clang" if subprocess.run(["which", "clang"], capture_output=True).returncode == 0 else "gcc"


def compile_and_run(extra_defs):
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / "test_arm_mpu"
        cmd = [
            CC,
            "-Wall",
            "-Wextra",
            "-Werror",
            "-std=gnu11",
            f"-I{ROOT}/tests/host/mm_block_stubs",
            f"-I{ROOT}/include",
            f"-I{ROOT}/tests/host/stubs",
            *extra_defs,
            str(ROOT / "tests/host/test_arm_mpu_platform.c"),
            str(ROOT / "kernel/mm/vmm_mpu.c"),
            "-o",
            str(exe),
        ]
        subprocess.run(cmd, check=True)
        subprocess.run([str(exe)], check=True)


def main():
    compile_and_run([])
    compile_and_run([
        "-DTEST_NUC472_LAYOUT",
        "-DCONFIG_ARM_MPU_FLASH_BASE=0x00000000UL",
        "-DCONFIG_ARM_MPU_FLASH_SIZE=0x80000UL",
        "-DCONFIG_ARM_MPU_SRAM_BASE=0x20000000UL",
        "-DCONFIG_ARM_MPU_SRAM_SIZE=0x10000UL",
    ])
    return 0


if __name__ == "__main__":
    sys.exit(main())
