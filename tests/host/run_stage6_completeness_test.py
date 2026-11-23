#!/usr/bin/env python3
"""运行 Stage 6 端到端宿主完备性集成测试（CDT/Cell/VMO/RingIPC/SMMUv3/AccelQueue/PIE）。"""

from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"


def main():
    with tempfile.TemporaryDirectory(prefix="nos-stage6-complete-test-") as tmp:
        exe = Path(tmp) / "test_stage6_completeness"
        subprocess.run(
            [
                os.environ.get("CC", "cc"),
                "-std=gnu11",
                "-O1",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-D__HOST_TEST__=1",
                "-DCONFIG_NOS_CAP_CDT=1",
                "-DCONFIG_NOS_AI_CORE=1",
                "-DCONFIG_NOS_CAP_MAX_SLOTS=32",
                "-I", str(TEST_DIR / "stubs"),
                "-I", str(ROOT / "include"),
                "-I", str(ROOT / "arch" / "arm64" / "include"),
                "-I", str(ROOT / "soc" / "rk3588" / "include"),
                str(TEST_DIR / "test_stage6_completeness.c"),
                str(ROOT / "kernel" / "ipc" / "object.c"),
                str(ROOT / "kernel" / "cap" / "cap_table.c"),
                str(ROOT / "kernel" / "mm" / "vmo.c"),
                str(ROOT / "kernel" / "ai" / "cell.c"),
                str(ROOT / "kernel" / "ipc" / "ring_ipc.c"),
                str(ROOT / "kernel" / "accel" / "accel_core.c"),
                str(ROOT / "drivers" / "accel" / "virtio_accel.c"),
                str(ROOT / "drivers" / "accel" / "rknpu.c"),
                str(ROOT / "kernel" / "loader" / "elf64_loader.c"),
                str(ROOT / "arch" / "arm64" / "mm" / "mmu.c"),
                str(ROOT / "arch" / "arm64" / "mm" / "smmuv3.c"),
                str(ROOT / "arch" / "arm64" / "kernel" / "fdt_scan.c"),
                "-lpthread",
                "-o", str(exe),
            ],
            check=True,
        )
        subprocess.run([str(exe)], check=True, timeout=20)


if __name__ == "__main__":
    main()
