#!/usr/bin/env python3
"""运行 USB CDC VCOM 驱动、日志后端与 TTY 桥接的主机回归测试。"""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "host"


def write_stub_headers(stub_inc: Path) -> None:
    """生成隔离编译 cdc.c 所需的最小内核与架构桩头文件。"""
    (stub_inc / "kernel").mkdir(parents=True, exist_ok=True)
    (stub_inc / "nos").mkdir(parents=True, exist_ok=True)
    (stub_inc / "asm").mkdir(parents=True, exist_ok=True)

    (stub_inc / "kernel" / "kernel.h").write_text(
        "#ifndef NOS_CDC_TEST_KERNEL_H\n"
        "#define NOS_CDC_TEST_KERNEL_H\n"
        "#include_next <kernel/kernel.h>\n"
        "#ifndef ARRAY_SIZE\n"
        "#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))\n"
        "#endif\n"
        "#ifndef __aligned\n"
        "#define __aligned(n) __attribute__((aligned(n)))\n"
        "#endif\n"
        "#ifndef __maybe_unused\n"
        "#define __maybe_unused __attribute__((unused))\n"
        "#endif\n"
        "#ifndef __visible\n"
        "#define __visible __attribute__((used))\n"
        "#endif\n"
        "#ifndef BIT\n"
        "#define BIT(n) (1UL << (n))\n"
        "#endif\n"
        "#endif\n",
        encoding="utf-8",
    )

    (stub_inc / "kernel" / "clk.h").write_text(
        "#ifndef NOS_CDC_TEST_CLK_H\n"
        "#define NOS_CDC_TEST_CLK_H\n"
        "#include <stdint.h>\n"
        "static inline uint32_t msec_to_tick(uint32_t msec)\n"
        "{\n"
        "    return msec == 0U ? 1U : msec;\n"
        "}\n"
        "static inline uint32_t tick_to_msec(uint32_t tick)\n"
        "{\n"
        "    return tick;\n"
        "}\n"
        "#endif\n",
        encoding="utf-8",
    )

    (stub_inc / "kernel" / "init.h").write_text(
        "#ifndef NOS_CDC_TEST_INIT_H\n"
        "#define NOS_CDC_TEST_INIT_H\n"
        "#define INIT_LEVEL_POST_KERNEL 3\n"
        "typedef int (*sys_init_entry_t)(void);\n"
        "void test_register_sys_init(sys_init_entry_t fn);\n"
        "#define SYS_INIT(fn, init_level) \\\n"
        "    static void __attribute__((constructor)) __cdc_ctor_##fn(void) \\\n"
        "    { \\\n"
        "        test_register_sys_init(fn); \\\n"
        "    }\n"
        "#endif\n",
        encoding="utf-8",
    )

    (stub_inc / "kernel" / "sleep.h").write_text(
        "#ifndef NOS_CDC_TEST_SLEEP_H\n"
        "#define NOS_CDC_TEST_SLEEP_H\n"
        "#include <kernel/types.h>\n"
        "void msleep(u32 msec);\n"
        "#endif\n",
        encoding="utf-8",
    )

    (stub_inc / "kernel" / "task.h").write_text(
        "#ifndef NOS_CDC_TEST_TASK_H\n"
        "#define NOS_CDC_TEST_TASK_H\n"
        "#include <stdint.h>\n"
        "struct task_struct {\n"
        "    const char *name;\n"
        "};\n"
        "struct task_struct *task_create(const char *name,\n"
        "                                void (*entry)(void *parameter),\n"
        "                                void *parameter,\n"
        "                                uint8_t priority,\n"
        "                                uint32_t stack_size,\n"
        "                                uint32_t tick,\n"
        "                                void (*clean)(struct task_struct *task));\n"
        "int task_ready(struct task_struct *task);\n"
        "#endif\n",
        encoding="utf-8",
    )

    # 复用内核真实 struct device 与 device_init/device_register 声明，避免 stubs/kernel/device.h 精简结构缺失字段
    (stub_inc / "kernel" / "device.h").write_text(
        (ROOT / "include" / "kernel" / "device.h").read_text(encoding="utf-8"),
        encoding="utf-8",
    )

    (stub_inc / "nos" / "completion.h").write_text(
        "#ifndef NOS_CDC_TEST_COMPLETION_H\n"
        "#define NOS_CDC_TEST_COMPLETION_H\n"
        "#include <stdbool.h>\n"
        "#include <stdint.h>\n"
        "#include <nos/timeout.h>\n"
        "struct nos_completion {\n"
        "    uint32_t done;\n"
        "    bool closed;\n"
        "};\n"
        "int nos_completion_init(struct nos_completion *comp);\n"
        "int nos_completion_wait(struct nos_completion *comp, nos_timeout_t timeout);\n"
        "int nos_complete(struct nos_completion *comp);\n"
        "int nos_complete_all(struct nos_completion *comp);\n"
        "int nos_completion_reset(struct nos_completion *comp);\n"
        "#endif\n",
        encoding="utf-8",
    )


def run_test() -> None:
    """编译并执行 CDC 主机单元测试。"""
    with tempfile.TemporaryDirectory(prefix="nos-usb-cdc-test-") as tmp:
        tmp_path = Path(tmp)
        stub_inc = tmp_path / "stub_inc"
        write_stub_headers(stub_inc)
        exe = tmp_path / "test_usb_cdc"

        cmd = [
            os.environ.get("CC", "cc"),
            "-std=gnu11",
            "-g",
            "-O1",
            "-fsanitize=address",
            "-fno-builtin",
            "-Wall",
            "-Werror",
            "-Wno-incompatible-function-pointer-types",
            "-D_FORTIFY_SOURCE=0",
            "-DCONFIG_SYS_TICK_MS=1",
            "-DCONFIG_TTY=1",
            "-DCONFIG_USB_CDC_LOG_BACKEND=1",
            "-DCONFIG_LOG_DEFAULT_VCOM=1",
            "-DCONFIG_CONSOLE_CRLF=1",
            "-DCONFIG_LOG_BUFFERED=1",
            "-DCONFIG_LOG_FIFO_BUF_SIZE=256",
            "-DCONFIG_LOG_MESSAGE_SIZE=128",
            "-DCONFIG_USB_DEVICE_COMPOSITE=1",
            "-I",
            str(stub_inc),
            "-I",
            str(TEST_DIR / "stubs"),
            "-I",
            str(ROOT / "include"),
            "-I",
            str(ROOT / "drivers/usb/device/class"),
            str(ROOT / "drivers/usb/device/class/cdc.c"),
            str(ROOT / "kernel/printk/log.c"),
            str(ROOT / "kernel/printk/console_backend.c"),
            str(ROOT / "kernel/tty/tty.c"),
            str(ROOT / "libs/kfifo.c"),
            str(TEST_DIR / "test_usb_cdc.c"),
            "-o",
            str(exe),
        ]
        subprocess.run(cmd, check=True)
        subprocess.run([str(exe)], check=True, timeout=15)


class UsbCdcTest(unittest.TestCase):
    def test_usb_cdc(self):
        run_test()


def main():
    run_test()


if __name__ == "__main__":
    main()
