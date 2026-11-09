#!/usr/bin/env python3
"""Test the STM32 driver with a simulated USART and real kernel FIFO."""

from pathlib import Path
import os
import subprocess
import tempfile
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"


def run_test(extra_flags=None):
    if extra_flags is None:
        extra_flags = []
    with tempfile.TemporaryDirectory(prefix="nos-stm32-uart-test-") as tmp:
        exe = Path(tmp) / "test_stm32_uart"
        dma = ["-DCONFIG_UART_DMA", "-DCONFIG_UART_DMA_BUFFER_SIZE=4"] if "--dma" in extra_flags else []
        dt = ["-DCONFIG_DEVICE_TREE", "-DCONFIG_BOARD_QEMU_STM32"] if "--dt" in extra_flags else []
        crlf = ["-DCONFIG_CONSOLE_CRLF"] if "--crlf" in extra_flags else []
        log_size = "4" if crlf else "8"
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=gnu11", "-O1",
             "-DCONFIG_SOC_STM32F4", "-DCONFIG_UART_RX_BUFFER_SIZE=8",
             "-DCONFIG_UART_TX_BUFFER_SIZE=8", f"-DCONFIG_LOG_MESSAGE_SIZE={log_size}", *dma, *dt, *crlf,
             "-I", str(TEST_DIR / "stubs"), "-I", str(ROOT / "include"),
             str(TEST_DIR / "test_stm32_uart.c"), str(ROOT / "libs/kfifo.c"),
             "-o", str(exe)], check=True,
        )
        subprocess.run([str(exe)], check=True)


class Stm32UartTest(unittest.TestCase):
    def test_stm32_uart(self):
        run_test([])

    def test_stm32_uart_dma(self):
        run_test(["--dma"])

    def test_stm32_uart_dt(self):
        run_test(["--dt"])


def main():
    run_test(sys.argv[1:])


if __name__ == "__main__":
    main()
