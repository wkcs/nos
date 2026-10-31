#!/usr/bin/env python3
"""Compile and run the NLK (NOS Link Kit) distributed soft-bus host regression suite."""

from pathlib import Path
import os
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"
NLK_DIR = ROOT / "app" / "server" / "nlk"


def main():
    with tempfile.TemporaryDirectory(prefix="nos-nlk-test-") as tmp:
        exe = Path(tmp) / "test_nlk"
        srcs = [
            ROOT / "kernel/printk/log.c",
            ROOT / "kernel/printk/console_backend.c",
            ROOT / "kernel/tty/tty.c",
            ROOT / "libs/kfifo.c",
            NLK_DIR / "core/nlk_frame.c",
            NLK_DIR / "core/nlk_discovery.c",
            NLK_DIR / "core/nlk_router.c",
            NLK_DIR / "core/nlk_core.c",
            NLK_DIR / "caps/cap_flash.c",
            NLK_DIR / "caps/cap_log.c",
            NLK_DIR / "caps/cap_shell.c",
            NLK_DIR / "caps/cap_vfs.c",
            NLK_DIR / "caps/cap_keyboard.c",
            NLK_DIR / "caps/cap_encoder.c",
            NLK_DIR / "caps/cap_display.c",
            NLK_DIR / "caps/cap_wireless.c",
            NLK_DIR / "bearers/bearer_bus_generic.c",
            TEST_DIR / "test_nlk.c",
        ]
        cmd = [
            os.environ.get("CC", "cc"),
            "-std=gnu11",
            "-O1",
            "-Wall",
            "-Wextra",
            "-DNLK_HOST_TEST",
            "-DCONFIG_LOG_BUFFERED",
            "-DCONFIG_LOG_FIFO_BUF_SIZE=512",
            "-DCONFIG_LOG_MESSAGE_SIZE=128",
            "-I", str(TEST_DIR / "stubs"),
            "-I", str(ROOT / "include"),
            "-I", str(NLK_DIR),
            *[str(s) for s in srcs],
            "-o", str(exe),
        ]
        subprocess.run(cmd, check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
