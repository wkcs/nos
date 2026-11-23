#!/usr/bin/env python3
"""验证 ELF64 严格 W^X 校验、Cell VMO 映射加载及安全越界拦截主机单元测试。"""

from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"


def main():
    with tempfile.TemporaryDirectory(prefix="nos-elf64-loader-test-") as tmp:
        exe = Path(tmp) / "test_elf64_loader"
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
                str(TEST_DIR / "test_elf64_loader.c"),
                "-I", str(ROOT / "arch" / "arm64" / "include"),
                str(ROOT / "kernel" / "ipc" / "object.c"),
                str(ROOT / "kernel" / "cap" / "cap_table.c"),
                str(ROOT / "kernel" / "mm" / "vmo.c"),
                str(ROOT / "kernel" / "ai" / "cell.c"),
                str(ROOT / "kernel" / "loader" / "elf64_loader.c"),
                str(ROOT / "arch" / "arm64" / "mm" / "mmu.c"),
                "-o", str(exe),
            ],
            check=True,
        )
        subprocess.run([str(exe)], check=True, timeout=15)


if __name__ == "__main__":
    main()
