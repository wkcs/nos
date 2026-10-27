# NOS 项目 Agent 指南

本文档为在 NOS (Nick Operating System) 代码库上工作的 AI Agent 提供详细的说明和上下文。旨在确保代码的一致性、稳定性以及遵循项目既定的模式。

## 1. 项目概述与环境

- **项目类型:** 嵌入式微内核/RTOS 操作系统。
- **主要语言:** C (标准 C99/GNU11), 汇编 (`arm`, `arm64`, `riscv`, `x86_64`)。
- **构建系统:** 纯 CMake + Ninja + Kconfig (`scripts/kconfiglib.py`)，支持 Windows / macOS / Linux 跨平台构建。
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
*   **OpenOCD:** `cmake --build build/<board> --target flash`
*   **ST-Link:** `cmake --build build/<board> --target stflash`
*   **QEMU 仿真:** `cmake --build build/qemu-stm32 --target qemu-run`

### 2.3 测试与验证
*   **单元测试:** 根目录下没有可见的集中式单元测试框架 (如 Unity/GoogleTest)。
    *   *策略:* 尽可能依赖编译成功 (`make`) 和通过 QEMU 进行运行时验证 (`make qemu-run`)。
    *   *CI:* `.gitlab-ci.yml` 使用占位符 echo 命令进行测试。不要寻找 `npm test` 或 `ctest`。
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
*   `arch/` - 架构特定代码 (ARM 等)。
*   `board/` - 板级支持包和配置。
*   `drivers/` - 设备驱动 (USB, I2C, SPI 等)。
*   `include/` - 头文件。`include/kernel/` 包含核心 API。
*   `init/` - 系统初始化 (`main.c`, `init.c`)。
*   `kernel/` - 核心内核逻辑 (调度器, IPC, 内存)。
*   `scripts/` - 用于构建/配置的 Python 和 Shell 脚本。

## 5. Git 提交规范
*   **格式:** `Component: Short description` (模块: 简短描述)
*   **示例:** `USB: Add timeout mechanism` 或 `Shell: Add nshell service`。
*   **语言:** 虽然仓库包含中文，但为了国际协作，英语优先。如果不确定，请匹配用户的语言。

## 6. Agent 工作流程
1.  **阅读:** 务必阅读 `Makefile` 和与任务相关的 `include/kernel/*.h` 以了解可用 API。
2.  **计划:** 在创建新驱动之前，先检查 `drivers/` 中是否已有驱动，避免重复。
3.  **编辑:** 保持 `snake_case` 命名和 4 空格缩进。
4.  **验证:** 由于无法运行硬件测试，请仔细验证语法和逻辑。建议用户运行 `make <board>_config && make` 进行验证。

---
*由 Antigravity 生成*
