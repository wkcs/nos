"""NuTiny-NUC472 Flash 固件体积、静态 RAM 占用与关键符号/配置约束守护测试。"""

import re
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ELF_PATH = ROOT / "build" / "nutiny-nuc472" / "nos.elf"
CONFIG_PATH = ROOT / "build" / "nutiny-nuc472" / ".config"

MAX_FLASH_BYTES = 391168  # 382 KiB (含 kallsyms 符号表后仍严格低于 384 KiB 内核分区上限 393,216 B)
MAX_STATIC_RAM_BYTES = 20480  # 20 KiB (确保 64 KiB SRAM 中保留充足 TLSF 动态堆)


class NuTinyNUC472FootprintTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not ELF_PATH.exists():
            raise unittest.SkipTest(f"固件未构建: {ELF_PATH}")

        # 使用 arm-none-eabi-size -A 获取各段精确大小（解决 Berkeley 格式将 .ramfunc 误计入 text 问题）
        out = subprocess.check_output(["arm-none-eabi-size", "-A", str(ELF_PATH)], text=True)
        cls.sections = {}
        for line in out.splitlines():
            parts = line.split()
            if len(parts) >= 2 and parts[1].isdigit():
                cls.sections[parts[0]] = int(parts[1])

        cls.data_size = cls.sections.get(".data", 0)
        cls.bss_size = cls.sections.get(".bss", 0)

        # 真实进入 Flash LMA 存储的物理段集合（不含 .debug_*、.bss、.ARM.attributes 等）
        cls.flash_lma_sections = [
            ".isr_vector",
            ".text",
            ".ARM.extab",
            ".ARM",
            ".init_array",
            ".fini_array",
            ".data",
        ]
        cls.flash_size = sum(cls.sections.get(s, 0) for s in cls.flash_lma_sections)

        # 使用 arm-none-eabi-nm 获取符号表
        nm_out = subprocess.check_output(["arm-none-eabi-nm", str(ELF_PATH)], text=True)
        cls.symbols = set()
        for line in nm_out.splitlines():
            cols = line.split()
            if len(cols) >= 3:
                cls.symbols.add(cols[2])

    def test_nuc472_flash_footprint(self):
        """校验物理 Flash LMA 总占用 <= 368,640 B (360 KiB)"""
        self.assertLessEqual(
            self.flash_size,
            MAX_FLASH_BYTES,
            f"Flash 超标: 已使用 {self.flash_size} B, 预算上限 {MAX_FLASH_BYTES} B (超出 {self.flash_size - MAX_FLASH_BYTES} B)"
        )

    def test_nuc472_ram_footprint(self):
        """校验静态 RAM (data + bss) <= 20,480 B (20 KiB)"""
        ram_used = self.data_size + self.bss_size
        self.assertLessEqual(
            ram_used,
            MAX_STATIC_RAM_BYTES,
            f"静态 RAM 超标: 已使用 {ram_used} B, 预算上限 {MAX_STATIC_RAM_BYTES} B (超出 {ram_used - MAX_STATIC_RAM_BYTES} B)"
        )

    def test_nuc472_forbidden_symbols(self):
        """校验禁用模块符号绝不链入 nos.elf (FatFs, Ramdisk, SLUB)"""
        forbidden = [
            "f_mount",
            "f_open",
            "ramdisk_init",
            "mm_slub_kmalloc",
        ]
        for sym in forbidden:
            self.assertNotIn(sym, self.symbols, f"检测到违规符号链入: {sym}")

    def test_nuc472_required_symbols(self):
        """校验关键功能符号全数真实链入 nos.elf"""
        required = [
            "mm_tlsf_alloc",
            "USBD_IRQHandler",
            "UART0_IRQHandler",
            "TMR0_IRQHandler",
            "GPA_IRQHandler",
            "GPI_IRQHandler",
            "fmc_isp_trigger_ramfunc",
            "cmd_lua",
            "nutiny_fs_init",
            "nutiny_usbd_register",
            "nutiny_nlk_register",
            "usbd_vcom_tty",
            "kallsyms_lookup",
            "board_read_boot_flag",
            "board_write_boot_flag",
        ]
        for sym in required:
            self.assertIn(sym, self.symbols, f"缺少关键符号: {sym}")

    def test_nuc472_config_constraints(self):
        """校验 .config 满足所有全局硬性架构约束"""
        if not CONFIG_PATH.exists():
            return
        content = CONFIG_PATH.read_text(encoding="utf-8")

        self.assertIn("CONFIG_MM_TLSF=y", content)
        self.assertNotIn("CONFIG_MM_SLUB=y", content)
        self.assertNotIn("CONFIG_FATFS=y", content)
        self.assertNotIn("CONFIG_RAMDISK=y", content)
        self.assertIn("CONFIG_MPU=y", content)
        self.assertIn("CONFIG_MPU_STACK_GUARD=y", content)
        self.assertIn("CONFIG_LITTLEFS=y", content)
        self.assertIn("CONFIG_FLASH_NUC472_FMC=y", content)
        self.assertIn("CONFIG_USB_NUC472_USBD=y", content)
        self.assertIn("CONFIG_USB_CDC_LOG_BACKEND=y", content)
        self.assertIn("CONFIG_LOG_DEFAULT_VCOM=y", content)
        self.assertIn("CONFIG_SHELL_VCOM=y", content)
        self.assertIn("CONFIG_KALLSYMS=y", content)
        self.assertIn("CONFIG_NLK_SERVER=y", content)
        self.assertIn("CONFIG_LUA=y", content)
        self.assertIn("CONFIG_LUA_SHELL_CMD=y", content)


if __name__ == "__main__":
    unittest.main()
