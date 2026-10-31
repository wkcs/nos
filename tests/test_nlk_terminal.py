"""NLK UAPI 协议契约、BootFlag 内核接口与分布式软总线主机回归测试。"""
import pathlib
import subprocess
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class NLKProtocolAndRegressionTest(unittest.TestCase):
    def test_nlk_tool_py_is_removed(self):
        """确保旧版 scripts/nlk_tool.py 已被移除，统一使用独立 nlk_cli。"""
        self.assertFalse((ROOT / "scripts" / "nlk_tool.py").exists())
        self.assertFalse((ROOT / "app" / "server" / "nlk" / "caps" / "cap_fw_update.c").exists())

    def test_kernel_boot_flag_contract(self):
        """验证内核级 boot_flag 定义位于 include/kernel/boot.h。"""
        boot_h = (ROOT / "include" / "kernel" / "boot.h").read_text(encoding="utf-8")
        self.assertIn("BOOT_FLAG_MAGIC", boot_h)
        self.assertIn("BOOT_NORMAL", boot_h)
        self.assertIn("BOOT_BOOTLOADER", boot_h)
        self.assertIn("BOOT_RECOVERY", boot_h)
        self.assertIn("write_boot_flag", boot_h)
        self.assertIn("read_boot_flag", boot_h)

    def test_nlk_uapi_capabilities_contract(self):
        """验证 NLK UAPI 包含通用 Flash、Log、Shell、VFS 和 Core Reboot 契约。"""
        nlk_h = (ROOT / "include" / "uapi" / "nos" / "nlk.h").read_text(encoding="utf-8")
        self.assertIn("NLK_CORE_OP_REBOOT", nlk_h)
        self.assertIn("NLK_CAP_FLASH", nlk_h)
        self.assertIn("NLK_CAP_LOG", nlk_h)
        self.assertIn("NLK_CAP_SHELL", nlk_h)
        self.assertIn("NLK_CAP_VFS", nlk_h)
        self.assertIn("NLK_SHELL_OP_EXEC", nlk_h)
        self.assertIn("NLK_SHELL_STREAM_EXIT", nlk_h)
        self.assertIn("NLK_VFS_OP_READDIR", nlk_h)
        self.assertNotIn("NLK_CAP_FW_UPDATE", nlk_h)

    def test_shared_nshell_has_no_remote_branch(self):
        """验证 UART 与 NLK 共用同一套 nshell 服务，无 remote 特判分支。"""
        nshell_c = (ROOT / "app" / "shell" / "nshell.c").read_text(encoding="utf-8")
        self.assertIn("nshell_spawn(", nshell_c)
        self.assertNotIn("shell->remote", nshell_c)

    def test_nlk_host_suite_passes(self):
        """运行 C 语言主机端 NLK 全能力（Flash/Log/Shell/VFS/路由）回归测试。"""
        subprocess.run(
            [sys.executable, str(ROOT / "tests" / "host" / "run_nlk_test.py")],
            check=True,
        )


if __name__ == "__main__":
    unittest.main()
