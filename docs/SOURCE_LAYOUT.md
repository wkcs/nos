# 源码结构

<!-- Copyright (C) 2026 NOS contributors -->

仓库只维护当前微内核构建。源码按职责组织，生成文件、工具链构建与验证报告统一放在忽略的 `out/` 下。

| 目录 | 职责 |
|---|---|
| `kernel/core/` | 系统调用分派、启动、内核运行库、进程凭据 |
| `kernel/sched/` | 任务生命周期、调度与睡眠 |
| `kernel/ipc/` | IPC、IRQ 对象、互斥锁与优先级继承 |
| `kernel/mm/` | 物理页、地址空间、用户复制、虚拟映射事务 |
| `kernel/exec/` | ELF 装载与启动文件 |
| `kernel/compat/linux/` | Linux 系统调用、进程与信号兼容机制 |
| `arch/{arm,arm64,riscv,riscv32}/` | 启动汇编、平台实现与链接脚本 |
| `include/nos/` | 当前内核、用户 ABI 与运行库声明 |
| `servers/` | 用户态 VFS、FatFs、UART 与 virtio 服务 |
| `servers/keyboard/`、`servers/usb/` | 有界键盘动作/配置引擎、板级服务与 USB HID 协议 |
| `boards/nk60_v2/` | 实板 F401 启动/链接/烧录配置，用户态 HC595、NOR、RGB 与 USB 驱动 |
| `user/init/` | 普通启动入口 |
| `user/shell/` | nshell 解析器、行编辑器、命令表与交互循环 |
| `user/lib/` | syscall、字符串、IPC、路径、文件 I/O、凭据、时钟、线程与 poll 包装 |
| `user/compat/linux/` | 独立链接的 Linux 用户态 syscall gateway |
| `libc/port/` | Newlib 与 musl 的 OS 适配 |
| `apps/` | 独立应用入口、示例、链接脚本、Lua/LVGL 应用 |
| `tests/guest/` | 内建来宾回归、架构探针；`apps/` 子目录为独立测试 ELF 源码 |
| `tests/` | 宿主消毒器、QEMU runner、验收编排与硬件 runner |
| `third_party/` | 保留许可证的 Lua/LVGL 第三方源码 |
| `scripts/`、`configs/` | 当前构建、环境准备与平台配置 |

函数实现置于所属模块的 `.c` 中。用户运行库拆成独立编译单元，模块间通过现有公共声明调用，不以包含 `.c` 的方式拼接。nshell 的纯解析/编辑接口在 `include/nos/shell.h`，命令执行私有声明在 `user/shell/commands.h`。VFS 现有静态内部实现仍使用本地私有头，其跨模块拆分可后续独立进行。

链接脚本按 `kernel/`、`arch/` 与用户态对象路径划分段；`tests/guest/` 明确归入用户段。新增目录时必须保持这个特权边界，并运行真实架构验证。

NK60 的 `boards/nk60_v2/boot.c` 为特权时钟/早期 UART/TIM2 初始化；同目录其余驱动明确链接入用户段，不依靠目录名推断权限。NK60 专用启动不链接通用 VFS/init/nshell，详见 [板级开发与验收](NK60_V2.md)。

本次删除旧 RTOS 的 `app/`、`board/`、`drivers/`、`fs/`、`init/`、`libs/`、旧头文件、旧内核/架构实现、Kconfig/旧构建和调试脚本、历史 QEMU 补丁、已跟踪磁盘镜像及过时的 Kconfig/编译器说明。Lua/LVGL 从旧应用目录迁出，当前代码不再依赖上述目录。未迁移的真实板级外设不因此获得支持。

Git 历史保留删除前内容。`DEVELOPMENT_PLAN.md` 是早期审查记录，其中旧路径和行号只对应当时版本；当前定位以此文档和构建源清单为准。
