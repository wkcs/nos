# NOS 项目 Agent 指南

本文档为在 NOS (Nick Operating System) 代码库上工作的 AI Agent 提供详细的说明和上下文。旨在确保代码的一致性、稳定性以及遵循项目既定的模式。

## 1. 项目概述与环境

- **项目类型:** 嵌入式微内核/RTOS 操作系统。
- **主要语言:** C (标准 C99/GNU11), 汇编 (`arm`, `arm64`, `riscv`, `x86_64`)。
- **目标架构:** `arm` (Cortex-M3/M4), `arm64` (ARMv8-A), `riscv` (RV32/RV64), `x86` (`x86_64`)。
- **构建系统:** 100% 纯 **CMake + Ninja + Kconfiglib** (`scripts/kconfiglib.py`)，无任何 `Makefile` 或 Unix Shell 依赖，原生支持 Windows / macOS / Linux 跨平台构建。
- **工具链:** `arm-none-eabi-gcc`（默认 MCU 工具链）、`aarch64-none-elf-gcc`、`riscv64-unknown-elf-gcc`、`x86_64-elf-gcc`。
- **架构蓝图:** 详见 `docs/ARCHITECTURE_AND_BUILD_SPEC.md`。

## 2. 构建、Lint 和测试命令

### 2.1 构建系统 (CMake + Ninja)
项目使用 `CMakePresets.json` 与 `configs/*_defconfig` 管理多架构与板级构建。

*   **使用 Preset 配置与构建 (推荐，Windows / macOS / Linux 通用):**
    *   `cmake --preset nk60-v2 && cmake --build --preset nk60-v2`
    *   `cmake --preset qemu-stm32 && cmake --build --preset qemu-stm32`
    *   `cmake --preset wl-001 && cmake --build --preset wl-001`
    *   `cmake --preset zj-v3 && cmake --build --preset zj-v3`

*   **手动指定参数构建:**
    *   `cmake -B build/<board> -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/arm-none-eabi.cmake -DNOS_DEFCONFIG=configs/<board>_defconfig`
    *   `cmake --build build/<board>`

*   **清理:**
    *   `cmake --build build/<board> --target clean` 或直接删除 `build/<board>` 目录。

### 2.2 烧录与调试
*   **OpenOCD:** `cmake --build --preset <board> --target flash`
*   **ST-Link:** `cmake --build --preset <board> --target stflash`
*   **QEMU 仿真:** `cmake --build --preset qemu-stm32 --target qemu-run`
*   **QEMU GDB 调试:** `cmake --build --preset qemu-stm32 --target qemu-run-gdb`
*   **Kconfig 配置:** `cmake --build --preset <board> --target menuconfig`

### 2.3 测试与验证
*   **主机回归:** `python3 tests/run_host_regressions.py`、`python3 -m unittest discover -s tests -p 'test_*.py'` 和 `cmake -P tests/test_arch_flags.cmake`。
*   **架构边界:** `python3 scripts/check_layer_deps.py`。
*   **板级验证:** 四个预设（`nk60-v2`、`qemu-stm32`、`zj-v3`、`wl-001`）均执行 `cmake --preset <board> && cmake --build --preset <board>`，再运行 `python3 tests/qemu_stm32_smoke.py build/qemu-stm32/nos.elf`。
*   **CI:** `.gitlab-ci.yml` 执行上述主机回归、板级构建和 QEMU 冒烟测试。
*   **Linting:** 目前未配置自动化的 Linter。
    *   *策略:* 你必须严格手动执行第 3 节中定义的代码风格。
    *   *验证:* 确保不引入编译器警告 (通常开启了 `-Wall`)。

## 3. 代码风格与规范

### 3.1 格式化
*   **缩进:** 4 个空格。**禁止使用 Tab。**
*   **行尾:** Unix 风格 (`\n`)。
*   **大括号风格:** K&R (内核风格)。
    ```c
    // 正确
    if (x == y) {
        action();
    } else {
        other_action();
    }

    // 函数定义
    int my_function(void)
    {
        return 0;
    }
    ```
*   **空格:**
    *   关键字后加空格 (`if`, `for`, `while`, `switch`)。
    *   函数调用名后不加空格: `my_func(arg)`。
    *   二元运算符周围加空格: `a = b + c;`。
    *   指针 `*` 主要绑定到变量名，尽管项目中有混用。优先使用 `struct task_struct *core`。

### 3.2 命名规范
*   **变量:** `snake_case` (例如: `task_stack`, `buffer_ptr`)。
*   **函数:** `snake_case` (例如: `task_create`, `usb_class_register`)。
*   **宏:** `UPPER_SNAKE_CASE` (例如: `USB_CLASS_DEVICE`, `CONFIG_MAX_PRIORITY`)。
*   **类型/结构体:** `snake_case` (例如: `struct usb_descriptor`)。
*   **Typedefs:** `snake_case` 通常以 `_t` 结尾 (例如: `init_task_t`, `udesc_t`, `addr_t`)。
    *   *注意:* `u32`, `u16`, `u8` 简写类型常与 `uint32_t` 混用。
*   **文件:** `snake_case.c` 和 `snake_case.h`。

### 3.3 类型与内存
*   **整数类型:** 优先使用 `<stdint.h>` 中的定宽类型或内核别名 (`u32`, `uint8_t`, `addr_t`)。
    *   *铁律:* `arch/*/include/asm/types.h` 中的 `s8/u8/s16/u16/s32/u32/s64/u64` 必须直接 `typedef` 自 `<stdint.h>` 的 `int8_t..uint64_t`，严禁硬编码为 `int`/`long`，以免与工具链 Newlib/Picolibc 或 CMSIS 头文件冲突。
*   **头文件隔离原则:**
    *   **严禁**在 `include/` 根目录下创建 `stdio.h`、`string.h`、`stdlib.h`、`stdarg.h`、`limits.h`、`signal.h` 等标准 C 库同名头文件。
    *   内核与用户态共享的 ABI 定义统一放在 `include/uapi/nos/*.h`（通过 `<uapi/nos/*.h>` 引用），且严禁任何 `#ifdef` 改变 UAPI 结构体（如 64 字节 `struct nos_ipc_msg`）的大小。
    *   微内核内部接口放在 `include/nos/*.h`（通过 `<nos/*.h>` 引用）。
*   **指针:** 显式检查 `NULL`。
    ```c
    if (ptr == NULL) {
        return -EINVAL;
    }
    ```
*   **内存:** 核心内核结构体首选静态分配。如果可用，应谨慎使用动态分配。

### 3.4 注释与文档
*   **文件头:** 必须包含版权和作者块。
    ```c
    /**
     * Copyright (C) 2023-2024 Nick Hu <email>
     * ...
     */
    ```
*   **风格:** C 风格 `/* ... */` 是标准。C++ 风格 `//` 可用于行尾注释或临时禁用代码。
*   **内容:** 记录复杂逻辑存在的*原因*，而不仅仅是它做了*什么*。

### 3.5 错误处理
*   **返回码:** 0 表示成功。负整数 (标准 errno 值，如 `-EINVAL`, `-ENOMEM`) 表示失败。
*   **内核日志:** 使用内核原语:
    *   `pr_info("System started\r\n");`
    *   `pr_err("Initialization failed\r\n");`
    *   `pr_fatal("Critical error\r\n");`
*   **断言:** 对不可恢复的系统状态使用 `BUG_ON(condition)`。

## 4. 源码目录结构
*   `cmake/` - CMake 构建模块（Kconfig 解析桥接、编译标志、跨架构工具链定义）。
*   `configs/` - 标准板级 `*_defconfig` 与配置片段。
*   `arch/` - 架构特定代码（`arm`, `arm64`, `riscv`, `x86`），统一实现 `include/nos/hal_arch.h` 契约。
*   `soc/` - SoC 芯片级外设库与系统初始化（`stm32f1`, `stm32f4`, `nuc472`，供各板级共享，禁止拷贝到 `board/` 下）。
*   `dts/` - SoC 设备树片段和设备属性 binding；板级 DTS 位于 `board/<board>/`。
*   `board/` - 板级支持包、引脚/时钟配置与链接脚本 `nos_kernel.ld.S`。
*   `include/` - 头文件：`include/uapi/nos/`（UAPI 契约层）、`include/nos/`（微内核与服务内部层）、`include/drivers/`（驱动接口）、`include/kernel/`（内核基础 API）。
*   `init/` - 系统初始化 (`main.c`, `init.c`)。
*   `kernel/` - 微内核核心逻辑（`task/`, `sched/`, `cap/`, `ipc/`, `syscall/`, `mm/`, `irq/`, `printk/`）。
*   `libs/` - 内核辅助库、标准 C 库端口 `libs/libc/` 和第三方库 `libs/third_party/`。
*   `user/` - 用户态/服务客户端库 (`user/libnos/`) 与 POSIX FD 桥接。
*   `drivers/` - 设备驱动 (UART, SPI, USB, Block 等)。
*   `fs/` - 虚拟文件系统 (VFS, FatFs, RamFS, ProcFS, SysFS)。
*   `app/` - 系统服务与应用程序 (nShell, Lua 5.4, Display Server, NDK)。
*   `scripts/` - 跨平台 Python 构建与固件打包脚本 (`gen_kconfig_cmake.py`, `mkkallsyms.py`, `create_img.py`)。

## 5. Git 提交规范
*   **格式:** `Component: Short description` (模块: 简短描述)
*   **示例:** `USB: Add timeout mechanism` 或 `Shell: Add nshell service`。
*   **语言:** 虽然仓库包含中文，但为了国际协作，英语优先。如果不确定，请匹配用户的语言。

## 6. Agent 工作流程
1.  **阅读:** 务必阅读 `CMakeLists.txt`、`docs/ARCHITECTURE_AND_BUILD_SPEC.md` 以及与任务相关的 `include/nos/*.h` / `include/kernel/*.h` 以了解可用 API。
2.  **计划:** 在创建新驱动之前，先检查 `drivers/` 中是否已有驱动，避免重复。
3.  **编辑:** 保持 `snake_case` 命名和 4 空格缩进。
4.  **验证:** 修改后务必使用 `cmake --preset <board> && cmake --build --preset <board>` 验证构建零错误、零新增警告。

---
*由 Antigravity 生成*
