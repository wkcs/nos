#!/usr/bin/env python3
"""Run focused host regressions for logging, serial and terminal code."""

from pathlib import Path
import subprocess
import sys


TEST_DIR = Path(__file__).resolve().parent / "host"
TESTS = [
    ("run_formatter_test.py",),
    ("run_printk_test.py",),
    ("run_log_sink_test.py",),
    ("run_log_filter_test.py",),
    ("run_log_ring_test.py",),
    ("run_console_test.py",),
    ("run_serial_api_test.py",),
    ("run_spi_api_test.py",),
    ("run_stm32_uart_test.py",),
    ("run_stm32_uart_test.py", "--dma"),
    ("run_tty_test.py",),
    ("run_tty_canonical_test.py",),
    ("run_spinlock_irq_test.py",),
    ("run_pl011_uart_test.py",),
    ("run_nlk_test.py",),
]


def main():
    for script, *arguments in TESTS:
        subprocess.run([sys.executable, str(TEST_DIR / script), *arguments], check=True)


if __name__ == "__main__":
    main()
