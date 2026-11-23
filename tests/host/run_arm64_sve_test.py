#!/usr/bin/env python3
"""验证 ARMv9 SVE 变长矢量惰性上下文切换与状态清理主机单元测试。"""

from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"


def main():
    with tempfile.TemporaryDirectory(prefix="nos-arm64-sve-test-") as tmp:
        inc_dir = Path(tmp) / "inc" / "asm"
        inc_dir.mkdir(parents=True)
        shutil.copy2(ROOT / "arch" / "arm64" / "include" / "asm" / "task.h", inc_dir / "task.h")

        exe = Path(tmp) / "test_arm64_sve"
        subprocess.run(
            [
                os.environ.get("CC", "cc"),
                "-std=gnu11",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-D__HOST_TEST__=1",
                "-DCONFIG_ARCH_ARM64=1",
                "-DCONFIG_NR_CPUS=4",
                "-DCONFIG_NOS_CAP_MAX_SLOTS=16",
                "-I", str(Path(tmp) / "inc"),
                "-I", str(TEST_DIR / "stubs"),
                "-I", str(ROOT / "include"),
                "-I", str(ROOT / "arch" / "arm64" / "include"),
                str(ROOT / "arch" / "arm64" / "cpu" / "fpu.c"),
                str(TEST_DIR / "test_arm64_sve.c"),
                "-o", str(exe),
            ],
            check=True,
        )
        subprocess.run([str(exe)], check=True, timeout=15)


if __name__ == "__main__":
    main()
