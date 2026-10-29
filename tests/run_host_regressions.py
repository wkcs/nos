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
    ("run_stm32_spi_test.py",),
    ("run_stm32_spi_test.py", "--f1"),
    ("run_sd_spi_test.py",),
    ("run_sd_spi_init_test.py",),
    ("run_sd_spi_io_test.py",),
    ("run_sd_csd_test.py",),
    ("run_sd_block_test.py",),
    ("run_i2c_api_test.py",),
    ("run_i2c_gpio_test.py",),
    ("run_devicetree_api_test.py",),
    ("run_dt_driver_test.py",),
    ("run_w25q_spi_test.py",),
    ("run_w25q_flash_test.py",),
    ("run_spi_dt_test.py",),
    ("run_zj_sd_dt_test.py",),
    ("run_stm32_uart_test.py",),
    ("run_stm32_uart_test.py", "--dt"),
    ("run_stm32_uart_test.py", "--dma"),
    ("run_tty_test.py",),
    ("run_tty_canonical_test.py",),
    ("run_spinlock_irq_test.py",),
    ("run_pl011_uart_test.py",),
    ("run_nlk_test.py",),
    ("run_shell_session_test.py",),
    ("run_shell_parse_test.py",),
    ("run_shell_glob_test.py",),
    ("run_shell_printf_test.py",),
    ("run_shell_pipe_test.py",),
    ("run_pid_test.py",),
    ("run_mm_block_test.py",),
    ("run_mutex_test.py",),
    ("run_task_stack_layout_test.py",),
    ("run_vfs_fd_lifetime_test.py",),
    ("run_vfs_fd_position_test.py",),
    ("run_ramfs_concurrency_test.py",),
]


def main():
    for script, *arguments in TESTS:
        subprocess.run([sys.executable, str(TEST_DIR / script), *arguments], check=True)


if __name__ == "__main__":
    main()
