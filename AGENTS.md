# NOS 项目 Agent 指南

本文档为在 NOS (Nick Operating System) 代码库上工作的 AI Agent 提供详细的说明和上下文。旨在确保代码的一致性、稳定性以及遵循项目既定的模式。

## 1. 项目概述与环境

- **项目类型:** 嵌入式操作系统 (类似 RTOS)。
- **主要语言:** C (标准 C99/GNU99), 汇编 (ARM)。
- **目标架构:** ARM (可通过 `ARCH` 配置，默认为 `arm`)。
- **构建系统:** GNU Make。
- **主机环境:** 已在 macOS (Darwin) 和 Linux 上验证。
- **工具链:** `arm-none-eabi-gcc` 是预期的交叉编译器。

## 2. 构建、Lint 和测试命令

### 2.1 构建系统
项目使用基于 Makefile 的构建系统。配置通过脚本处理。

*   **配置 (必须首先执行):**
    *   `make <board_name>_config`
    *   示例: `make nk60-v2_config`, `make zj-v3_config`
    *   *Agent 注意:* 如果用户未指定，请务必检查 `.gitlab-ci.yml` 或 `board/` 目录以获取有效的板级配置。

*   **构建项目:**
    *   `make` (构建内核和配置的模块)
    *   `make -j$(nproc)` (推荐并行构建)

*   **清理:**
    *   `make clean` (删除 `out/` 目录和构建产物)

### 2.2 烧录与调试
*   **OpenOCD:** `make flash` (使用 `openocd.cfg`)
*   **ST-Link:** `make stflash` (复位状态下连接)
*   **J-Link:** `make jflash` (使用 `scripts/jflash.sh`)
*   **GDB 调试:** `make debug` (启动 GDB 管道连接到 OpenOCD，在 `nos_start` 处断点)
*   **QEMU 仿真:** `make qemu-run` (如果支持，在 QEMU 中运行生成的 ELF)

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
