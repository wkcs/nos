"""NuTiny-NUC472 Flash 固件体积、静态 RAM 占用与关键符号/配置约束守护测试。"""

import re
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ELF_PATH = ROOT / "build" / "nutiny-nuc472" / "nos.elf"
CONFIG_PATH = ROOT / "build" / "nutiny-nuc472" / ".config"

MAX_FLASH_BYTES = 368640  # 360 KiB (相对 384 KiB 内核分区保留至少 24 KiB 余量)
MAX_STATIC_RAM_BYTES = 20480  # 20 KiB (确保 64 KiB SRAM 中保留充足 TLSF 动态堆)


class NuTinyNUC472FootprintTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not ELF_PATH.exists():
            raise unittest.SkipTest(f"固件未构建: {ELF_PATH}")

        # 使用 arm-none-eabi-size 获取段大小
        out = subprocess.check_output(["arm-none-eabi-size", str(ELF_PATH)], text=True)
        lines = out.strip().splitlines()
        cls.assertTrue(cls, len(lines) >= 2, "arm-none-eabi-size 输出格式异常")
        parts = lines[1].split()
        cls.text_size = int(parts[0])
        cls.data_size = int(parts[1])
        cls.bss_size = int(parts[2])

        # 使用 arm-none-eabi-nm 获取符号表
        nm_out = subprocess.check_output(["arm-none-eabi-nm", str(ELF_PATH)], text=True)
        cls.symbols = set()
        for line in nm_out.splitlines():
            cols = line.split()
            if len(cols) >= 3:
                cls.symbols.add(cols[2])

    def test_nuc472_flash_footprint(self):
        """校验 Flash (text + data) <= 368,640 B (360 KiB)"""
        flash_used = self.text_size + self.data_size
        self.assertLessEqual(
            flash_used,
            MAX_FLASH_BYTES,
            f"Flash 超标: 已使用 {flash_used} B, 预算上限 {MAX_FLASH_BYTES} B (超出 {flash_used - MAX_FLASH_BYTES} B)"
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
        self.assertIn("CONFIG_NLK_SERVER=y", content)
        self.assertIn("CONFIG_LUA=y", content)
        self.assertIn("CONFIG_LUA_SHELL_CMD=y", content)


if __name__ == "__main__":
    unittest.main()
