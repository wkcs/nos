# NOS (Nick Operating System)

NOS 是一个面向多架构（`arm`、`arm64`、`riscv`、`x86_64`）演进的混合微内核实时操作系统，兼具 MCU 单地址空间（SASOS）极速运行能力与 MMU 多进程微内核隔离能力，原生兼容标准 C 库（Newlib / Picolibc / musl）与 POSIX 接口。

完整的架构设计、四层头文件隔离规范、微内核 ABI 契约与构建系统说明见：
- [NOS 微内核、多架构、POSIX/Libc 兼容与 CMake+Kconfig 构建系统设计规范](docs/ARCHITECTURE_AND_BUILD_SPEC.md)

---

## 1. 核心特性

- **100% 纯 CMake + Ninja + Kconfiglib 跨平台构建**：彻底移除传统 `Makefile` 与 Unix Shell 依赖，在 **Windows (PowerShell / CMD / VS Code)**、**macOS** 和 **Linux** 上拥有完全一致的构建体验。
- **四层头文件隔离与零冲突标准库兼容**：
  - 编译器内置头（`<stdint.h>`、`<stdarg.h>` 等）与工具链标准 C 库头（`<stdio.h>`、`<string.h>`、`<stdlib.h>`、`<errno.h>`）零劫持、零重定义警告。
  - `libc/port/picolibc_newlib.c` 提供递归中断安全的 `__malloc_lock`、`kmalloc`/`kfree` 桥接以及基于控制台和 VFS 的 `_read_r`/`_write_r`/`_open_r`/`_close_r`/`_lseek_r`。
- **微内核核心机制**：
  - **代数句柄与 Capability 权限表**（`kernel/cap/cap_table.c`）：16 位代数计数器防止 ABA 悬空句柄，支持单调权限衰减（`NOS_CAP_RIGHT_*`）与跨任务授权（`NOS_CAP_RIGHT_GRANT`）。
  - **同步会合 Endpoint IPC**（`kernel/ipc/endpoint.c`）：固定 64 字节 `struct nos_ipc_msg` ABI（编译期静态断言保护，绝不随 `#ifdef` 改变大小），支持内核防伪造 `badge` 注入、随消息传递 Capability 句柄以及客户端到服务端的优先级继承。
  - **异步位图 Notification**（`kernel/ipc/notification.c`）：支持中断下半部（IRQ）与定时器的无锁/自旋锁位图事件投递。
  - **16 个核心微内核系统调用**（`kernel/syscall/syscall.c`）与客户端 SDK（`user/libnos/`）。
- **多架构统一 HAL 契约**（`include/nos/hal_arch.h`）：模块化支持 `arch/arm`（Cortex-M3/M4）、`arch/arm64`（ARMv8-A）、`arch/riscv`（RV32/RV64）与 `arch/x86`（`x86_64`）。

---

## 2. 快速开始（Windows / macOS / Linux 通用）

### 2.1 环境依赖
- **CMake** >= 3.20
- **Ninja** 构建器
- **Python 3**（用于执行仓库内置的 `scripts/kconfiglib.py`、`scripts/mkkallsyms.py` 与 `scripts/create_img.py`）
- **交叉编译工具链**：如 `arm-none-eabi-gcc`（ARM MCU）、`aarch64-none-elf-gcc`、`riscv64-unknown-elf-gcc` 或 `x86_64-elf-gcc`

### 2.2 使用 CMake Presets 一键构建（推荐）

```bash
# 1. NK60-v2 (STM32F401 Cortex-M4F 键盘主控板)
cmake --preset nk60-v2
cmake --build --preset nk60-v2

# 2. QEMU-STM32 (STM32F103 Cortex-M3 仿真金标板，含 VFS + FatFs + RamFS + Shell)
cmake --preset qemu-stm32
cmake --build --preset qemu-stm32

# 3. ZJ-v3 (STM32F103 Cortex-M3 开发板，含 Lua 5.4 + FatFs + USB + SPI SDCard)
cmake --preset zj-v3
cmake --build --preset zj-v3

# 4. WL-001 (STM32F103 Cortex-M3 节点板)
cmake --preset wl-001
cmake --build --preset wl-001
```

构建完成后，产物统一输出在 `build/<preset>/` 目录下：
- `nos.elf`：包含两遍扫描生成的完整内核符号表（`kallsyms`）的 ELF 可执行文件
- `nos.bin` / `nos.hex`：用于烧录的二进制与 Intel HEX 固件
- `nos.img`：带版本头的升级镜像文件
- `nos.list` / `nos.map`：反汇编清单与链接映射表

### 2.3 手动指定工具链与 Defconfig 构建

```bash
cmake -B build/nk60-v2 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/arm-none-eabi.cmake \
  -DNOS_DEFCONFIG=configs/nk60-v2_defconfig
cmake --build build/nk60-v2
```

### 2.4 仿真、烧录与配置管理

```bash
# 在 QEMU 中运行 qemu-stm32 固件（需支持 nos_stm32 机型的定版 QEMU）
cmake --build --preset qemu-stm32 --target qemu-run

# 启动 QEMU 并挂载 GDB 调试端口 (:1234)
cmake --build --preset qemu-stm32 --target qemu-run-gdb

# 使用 OpenOCD 或 ST-Link 烧录目标板
cmake --build --preset nk60-v2 --target flash
cmake --build --preset nk60-v2 --target stflash

# 打开交互式 Kconfig 菜单配置并保存最小化 defconfig
cmake --build --preset nk60-v2 --target menuconfig
cmake --build --preset nk60-v2 --target savedefconfig
```

---

## 3. 目录结构概览

```text
nos/
├── CMakeLists.txt             # 顶层 CMake 构建入口（含双遍 kallsyms 与固件打包）
├── CMakePresets.json          # 跨平台板级与多架构构建预设
├── Kconfig                    # 顶层 Kconfig 入口
├── cmake/                     # Kconfig 桥接、编译选项、辅助宏与跨架构 Toolchain
├── configs/                   # 板级 *_defconfig 与功能特性片段
├── docs/                      # 架构规范与重构设计文档
├── include/
│   ├── uapi/nos/              # 第 1 层：内核与用户态共享 ABI 契约头（types/errno/syscall/ipc/cap/abi）
│   ├── nos/                   # 第 2 层：微内核内部头（cap/ipc/syscall/hal_arch/klibc）
│   └── kernel/                # 内核基础服务与驱动兼容头文件
├── arch/                      # 多架构支持（arm / arm64 / riscv / x86）及统一 HAL 实现
├── soc/                       # 芯片级外设库与启动支持（stm32f1 / stm32f4）
├── board/                     # 板级配置、时钟/引脚初始化与链接脚本（nk60-v2 / qemu-stm32 / wl-001 / zj-v3）
├── kernel/                    # 微内核核心（task / sched / cap / ipc / syscall / mm / irq / printk）
├── libc/                      # 标准 C 库（Newlib / Picolibc）底层系统调用与锁移植层
├── user/                      # 用户态/客户端系统调用封装（libnos）与 POSIX FD 桥接
├── drivers/                   # 设备驱动框架与外设驱动（UART / SPI / USB / Block / Key / LED）
├── fs/                        # 虚拟文件系统（VFS / FatFs / RamFS / ProcFS / SysFS）
└── app/                       # 系统服务与应用（nShell / Lua 5.4 / Display Server / NDK）
```
