# NOS 微内核、多架构、POSIX/Libc 兼容与 CMake+Kconfig 构建系统设计规范

> **状态:** 正式实施蓝图 (Master Specification v1.0)
> **适用范围:** NOS (`/Volumes/nick-disk/projects/nos`) 演进与微内核重构 (`/Volumes/nick-disk/projects/codex/nos` 融合)
> **目标架构:** `arm` (ARMv7-M / ARMv7-A), `arm64` (ARMv8-A), `riscv` (RV32 / RV64), `x86` (`x86_64`)

---

## 1. 现状诊断与双仓对比总结

### 1.1 主流 RTOS 是如何解决标准库 (`libc`) 头文件与符号冲突的？
当前 `nos` 在 `include/` 根目录下放置了自定义的 `include/stdio.h`（将 `#define FILE int`、`fopen` 全部做成空桩）与 `include/string.h`，并在 `arch/arm/include/asm/types.h` 中将 `s32`/`u32` 硬编码为 `signed int`/`unsigned int`，同时在 `include/kernel/compiler-gcc.h` 中无条件 `#define __always_inline`、`__packed`、`__pure`、`__aligned`，在 `include/kernel/errno.h` 中使用与 Newlib 不同的错误码编号。这导致只要任何驱动或外设库引入工具链的 `<stdint.h>` 或 `<sys/types.h>`，就会触发：
1. **类型重定义冲突：** `arm-none-eabi-gcc` (Newlib) 中 `int32_t` 是 `long int`，而 `asm/types.h` 中 `s32` 是 `signed int`，一旦 CMSIS (`stm32f10x.h:486`) 执行 `typedef int32_t s32;` 直接编译报错。
2. **编译器属性宏重定义：** Newlib `<sys/cdefs.h>` 定义了 `__always_inline`、`__packed` 等宏，与 `include/kernel/compiler-gcc.h` 产生大量 `redefined` 警告。
3. **标准库头文件劫持：** `-Iinclude` 下的伪造 `stdio.h` 劫持了 `<stdio.h>`，使第三方组件（Lua、LVGL、POSIX 应用）既不能用真正的标准库流，也无法与 Newlib/Picolibc/musl 共存。
4. **`errno` 二义性：** 内核使用 Linux 编号（`ENOSYS=38`, `ETIMEDOUT=110`），而 Newlib `<sys/errno.h>` 使用 `ENOSYS=88`, `ETIMEDOUT=116`。

主流 RTOS（Zephyr、RT-Thread、NuttX、seL4）的通用解法是**严格物理隔离头文件搜索路径 + 统一编译器内置定宽类型 + 底层 Syscall Hook 对接**：
- **编译器内置头文件永远优先：** `<stdint.h>`, `<stddef.h>`, `<stdbool.h>`, `<stdarg.h>`, `<limits.h>` 100% 由编译器自身提供，OS 绝不伪造。
- **内核内部头文件命名空间化：** 内核内部只包含 `<nos/...>`（或 `<kernel/...>`），**严禁在全局 `-Iinclude` 根目录下放置 `stdio.h`、`stdlib.h`、`string.h`、`errno.h`、`sys/types.h`**。
- **分层编译选项：**
  - **微内核层 (`kernel/`)：** 使用 `-ffreestanding -nostdlib`，仅使用编译器内置头文件与 `<nos/klibc.h>`（提供 `kmemcpy`/`kmemset`/`ksnprintf` 等内核自包含实现），不依赖外部 `libc`。
  - **用户态服务与应用层 (`servers/`, `user/`, `apps/`)：** 根据 `CONFIG_LIBC_*` 选择 Picolibc / Newlib（MCU/No-MMU）或 musl libc（MMU/A-profile），通过 `libc/port/` 实现底层 `_read`/`_write`/`_sbrk`/`__malloc_lock` 或 `__syscall_cp`。

---

### 1.2 `nos`（原仓）与 `codex/nos`（微内核改造仓）优劣对比

| 维度 | `nos`（当前主仓） | `codex/nos`（微内核实验仓） | 最终融合方案（取长补短） |
| :--- | :--- | :--- | :--- |
| **头文件命名空间** | `include/stdio.h`、`include/string.h` 污染全局搜索路径；`s32` 与 Newlib `int32_t` 冲突 | `include/nos/*` 命名空间干净，无顶层伪造标准库头，使用 `-isystem` 引入 libc | **采纳 `codex/nos` 的 `include/nos/*` + 新增 `include/uapi/nos/*`**；修复 `s32`/`u32` 基于 `<stdint.h>` 定义 |
| **微内核核心机制** | 宏内核结构，VFS/驱动/应用全部运行在同一特权级 | 具备完整的代数句柄（Generation-counted Handle）、同步端点 IPC (`CALL`/`RECV`/`REPLY` + 优先级继承 + Token)、Capability 权限检查 (`MMIO`/`DMA`/`IRQ`) | **保留 `codex/nos` 的微内核 IPC、Handle、Capability 核心算法**，但拆分为职责单一的小文件 |
| **代码组织与宏控制** | 目录层次较清晰，但存在 `soc/stm32f*` 与 `board/*/drivers` 重复拷贝 | 严重的 `#ifdef` 宏污染（`NOS_LINUX_COMPAT`/`NOS_ESP_IDF`/`NOS_NK60_V2` 甚至改变 ABI 消息结构体大小）；6 个架构塞进单个 `include/nos/arch.h`；`memory.c` 2000+ 行、`vfs/server.c` 1100+ 行 | **严禁编译宏改变内核 ABI 结构体大小**；按架构目录物理拆分 `arch/<arch>/`；`kernel/mm` 与 `servers/vfs` 按子模块拆分 |
| **Libc 与 POSIX 策略** | 伪造 `stdio.h` + 局部 `libs/string.c` | 同时维护 `minimal`、`newlib`、`musl` 以及一套极其沉重的 Debian `glibc` + Linux 6 参数系统调用兼容层 (`kernel/compat/linux/`) | **砍掉 `glibc` + Linux 二进制仿真层**；MCU 采用 **Picolibc / Newlib**，MMU 采用 **musl libc**，POSIX 在 `user/posix/` 统一基于微内核 IPC 实现 |
| **构建与配置系统** | 手写递归 `Makefile.build` + 正则脚本 `kconfig_improved.py`（已损坏，跳过 `arch/arm/Kconfig` 且输出文件名不匹配） | 单一巨型 `Makefile`（500+ 行 `if/else`）+ Python 拼接 `autoconf.h`，无 Kconfig 依赖校验，单镜像靠 `*/user/*.o` 路径通配符区分特权级 | **全面切换至 CMake + Ninja + 原生 `kconfiglib`**；通过 CMake Target 属性显式控制编译选项与段前缀（不再靠路径通配符） |

---

## 2. 目标目录结构规范 (Target Directory Layout)

改造分阶段推进，最终态目录结构如下（兼顾当前 `nos` 平滑过渡与微内核演进）：

```text
nos/
├── CMakeLists.txt                 # 顶层 CMake 入口（支持内核、服务、用户态及固件打包）
├── CMakePresets.json              # Windows / macOS / Linux 跨平台一键构建预设
├── Kconfig                        # 顶层 Kconfig 入口（由 scripts/kconfiglib.py 解析）
├── cmake/                         # CMake 构建框架模块
│   ├── kconfig.cmake              # 调用 gen_kconfig_cmake.py 生成 .config / autocfg.h / kconfig.cmake
│   ├── compiler_flags.cmake       # 编译器通用与架构无关编译选项
│   ├── helpers.cmake              # nos_add_library / nos_add_user_module / kallsyms / 镜像生成宏
│   └── toolchains/                # 纯工具链定义（与架构/板级解耦）
│       ├── arm-none-eabi.cmake
│       ├── aarch64-elf.cmake
│       ├── riscv-elf.cmake
│       ├── x86_64-elf.cmake
│       └── clang-cross.cmake
├── configs/                       # 标准板级 defconfig 与特性片段 (Fragments)
│   ├── nk60-v2_defconfig
│   ├── qemu-stm32_defconfig
│   ├── wl-001_defconfig
│   ├── zj-v3_defconfig
│   ├── qemu-virt-arm64_defconfig
│   ├── qemu-virt-riscv64_defconfig
│   ├── qemu-q35-x86_64_defconfig
│   └── fragments/
│       ├── debug.conf
│       ├── posix.conf
│       └── microkernel.conf
├── include/
│   ├── uapi/                      # 【第 1 层】内核与用户态唯一共享的 ABI 契约层（纯 C，零内核私有依赖）
│   │   └── nos/
│   │       ├── types.h            # 基于 <stdint.h> 的固定宽度类型与句柄类型 (nos_handle_t)
│   │       ├── errno.h            # 统一 POSIX errno 编号（与 Newlib/Picolibc/musl 对齐）
│   │       ├── syscall.h          # 微内核 16 个核心系统调用号与参数结构
│   │       ├── ipc.h              # 固定大小 struct nos_ipc_msg（严禁随 #ifdef 变长）
│   │       ├── cap.h              # Capability 权限掩码与令牌结构 (MMIO/DMA/IRQ/IOPORT)
│   │       └── abi.h              # 进程启动信息、VFS/Proc/Net IPC 协议消息号
│   ├── nos/                       # 【第 2 层】微内核内部专用头文件（仅 kernel/ 与 arch/ 可见）
│   │   ├── klibc.h                # 内核内部自包含字符串/格式化函数声明
│   │   ├── compiler.h             # 安全防护（#ifndef）的编译器属性宏
│   │   ├── sched.h                # 调度器、就绪队列、优先级继承接口
│   │   ├── task.h                 # TCB / Process / Thread 对象定义
│   │   ├── ipc_endpoint.h         # 同步 IPC 端点与等待队列
│   │   ├── mm.h                   # 物理页帧、Slab/Kmem、vm_space 统一抽象
│   │   ├── irq.h                  # 中断路由与用户态 IRQ 通知对象
│   │   ├── cap_table.h            # Capability 句柄表与代数校验
│   │   └── hal_arch.h             # 跨架构统一 HAL 接口契约（禁止包含架构私有寄存器）
│   └── kernel/                    # 【过渡兼容层】现有驱动与内核代码使用的头文件，逐步收敛至 include/nos/
├── arch/                          # 【架构层】每个架构严格遵循 6 文件模块化契约
│   ├── Kconfig
│   ├── arm/                       # ARMv7-M (Cortex-M3/M4/M7, MPU) & ARMv7-A (Cortex-A7, MMU)
│   ├── arm64/                     # ARMv8-A (Cortex-A53/A72, EL0/EL1, MMU, GICv2/v3)
│   ├── riscv/                     # RV32IMAC (PMP) & RV64GC (Sv39/Sv48 MMU, PLIC/CLINT)
│   └── x86/                       # x86_64 (Ring0/Ring3, SYSCALL/SYSRET, 4-level Paging, APIC, IOPORT)
├── soc/                           # 【SoC 层】芯片级外设与启动代码（彻底消除 board/*/drivers 重复拷贝）
│   ├── Kconfig
│   ├── stm32f1/
│   ├── stm32f4/
│   └── qemu_virt/
├── board/                         # 【板级层】仅保留引脚复用、时钟树配置、内存布局参数与链接脚本
│   ├── Kconfig
│   ├── nk60-v2/
│   ├── qemu-stm32/
│   ├── wl-001/
│   ├── zj-v3/
│   └── qemu-virt/
├── kernel/                        # 【微内核层】仅保留机制，不含策略
│   ├── core/                      # 启动引导、对象句柄表 (handle.c)、系统调用分发 (syscall.c)、panic、kallsyms
│   ├── sched/                     # O(1) 位图 + 时间片轮转 + 优先级继承调度器
│   ├── task/                      # 线程/进程生命周期、TLS 上下文管理
│   ├── ipc/                       # 同步 Endpoint (CALL/RECV/REPLY)、异步 Notification、内核态 Mutex/Sem
│   ├── mm/                        # pmm.c (物理页/块)、kmem.c (内核小对象)、vmm_mmu.c / vmm_mpu.c (双模地址空间)
│   ├── irq/                       # 中断绑定与底半部事件投递
│   └── klibc/                     # 内核专用 string.c / vsprintf.c / kfifo.c
├── servers/                       # 【系统服务层】运行于用户态（或可配置同地址空间服务任务）
│   ├── procmgr/                   # 进程/服务生命周期管理、Capability 初始分发
│   ├── vfs/                       # 虚拟文件系统服务（mount.c / fd_table.c / ramfs.c / fatfs_srv.c / devfs.c）
│   ├── drv_framework/             # 用户态驱动框架（基于 CAP_MMIO / CAP_IRQ / CAP_DMA / CAP_IOPORT）
│   └── display/                   # 显示与输入服务
├── drivers/                       # 设备驱动（支持编译为内核早期驱动或用户态独立驱动服务）
├── user/                          # 【用户态 SDK 与 POSIX 兼容层】
│   ├── include/                   # 仅在不开启外部完整 libc 时或补充 POSIX 缺失头时通过 -isystem 引入
│   ├── libnos/                    # 微内核系统调用 C 封装（sys_ipc_call / sys_cap_grant / sys_vm_map 等）
│   └── posix/                     # 真正的 POSIX 适配层（将 open/read/write/pthread/socket/timer 转为 IPC）
│       ├── fd_io.c                # open/close/read/write/lseek/ioctl/fcntl/dup2 -> VFS Server IPC
│       ├── pthread.c              # pthread_create/join/mutex/cond/rwlock/key -> Kernel Thread + Futex/Mutex
│       ├── signal.c               # sigaction/kill/sigprocmask -> Procmgr Server + Kernel Exception Upcall
│       ├── time_clock.c           # clock_gettime/nanosleep/timer_create -> Kernel Tick & Timer Object
│       ├── mman.c                 # mmap/munmap/mprotect -> Kernel VM Space / VFS Server
│       └── poll_select.c          # poll/select/epoll -> Kernel Notification + VFS Server
├── libc/                          # 【标准库桥接层】
│   ├── Kconfig
│   └── port/
│       ├── picolibc_newlib.c      # 对接 arm-none-eabi / riscv-none-embed 的 _reent、_sbrk、__malloc_lock、stdio
│       └── musl_port.c            # 对接 musl libc 的 __syscall_cp 与 TLS 初始化
└── app/                           # 应用程序与测试套件（Shell、LVGL、Lua、业务应用）
```

---

## 3. 头文件分层与标准库/POSIX 兼容契约 (Header & Libc Contract)

### 3.1 四层头文件隔离法则
1. **层 0：编译器内置 Freestanding 头 (`<stdint.h>`, `<stddef.h>`, `<stdbool.h>`, `<stdarg.h>`, `<limits.h>`)**
   - 内核态与用户态均可直接 `#include`。
   - **铁律：** `arch/*/include/asm/types.h` 与 `include/uapi/nos/types.h` 中的 `s8/u8/s16/u16/s32/u32/s64/u64` **必须直接 `typedef` 自 `<stdint.h>` 的 `int8_t/uint8_t/int16_t/uint16_t/int32_t/uint32_t/int64_t/uint64_t`**。这样无论在 `arm-none-eabi-gcc`（`int32_t` 为 `long`）还是 `aarch64`/`x86_64`（`int32_t` 为 `int`）下，`s32` 与 `int32_t` 永远是完全相同的底层类型，彻底根除 CMSIS / HAL 库冲突。
2. **层 1：UAPI 契约头 (`include/uapi/nos/*.h`)**
   - 内核与用户态共享的唯一桥梁。
   - **铁律：** 只允许包含层 0 头文件；所有结构体字段必须使用定宽类型（`uint32_t`, `uint64_t`, `uintptr_t`）；**严禁任何 `#ifdef CONFIG_*` 或板级宏改变 UAPI 结构体的字段偏移或总大小**。
3. **层 2：内核私有头 (`include/nos/*.h` 及过渡期 `include/kernel/*.h`)**
   - 仅在编译 `kernel/`、`arch/`、`soc/`、`board/` 及内核态驱动时加入 `-I` 搜索路径。
   - **铁律：** `include/kernel/compiler-gcc.h` 中定义 `__always_inline`、`__packed`、`__pure`、`__aligned`、`__weak`、`__used` 等属性宏时，必须使用 `#ifndef __packed ... #endif` 保护，防止与 Newlib/Picolibc `<sys/cdefs.h>` 产生重定义警告。
   - **清理计划：** 逐步移除顶层 `include/stdio.h` 与 `include/string.h`。对于内核内部的 `memcpy`/`memset`/`strcmp`/`snprintf`，统一由 `<nos/klibc.h>` 提供声明；在过渡期保留 `include/string.h` 时，其函数签名必须与标准 C 完全一致。
4. **层 3：用户态 C 标准库与 POSIX 头**
   - **MCU 档（Cortex-M / RV32，无 MMU 或仅 MPU）：** 直接使用交叉工具链自带的 **Newlib-nano / Picolibc** 头文件（编译器默认 system include 路径），由 `libc/port/picolibc_newlib.c` 实现：
     - 每个任务 TCB 维护独立的 `struct _reent` 指针，在 `arch_context_switch` 时切换 `_impure_ptr`（或通过 `__getreent()` 返回 `current->tls_reent`）；
     - 实现递归安全的 `__malloc_lock` / `__malloc_unlock`；
     - 实现 `_read_r` / `_write_r` / `_open_r` / `_close_r` / `_lseek_r` / `_fstat_r` / `_sbrk_r`，无缝对接 NOS VFS（宏内核模式下直接调用 `vfs_*`，微内核模式下通过 `libnos` 发送 VFS IPC）。
   - **MMU 档（ARMv7-A / ARM64 / RV64 / x86_64）：** 采用 **musl libc**，完全遵循标准 POSIX 头文件，底层由 `user/posix/` + `libc/port/musl_port.c` 桥接至微内核 IPC，不再使用任何 Linux `glibc` 仿真层。

### 3.2 统一 `errno` 策略
- 当使用外部工具链 `libc`（Newlib/Picolibc/musl）时，`<errno.h>` 直接使用工具链提供的标准值；
- 内核 `include/uapi/nos/errno.h`（及 `include/kernel/errno.h`）在检测到已包含或启用工具链 `<sys/errno.h>` 时直接复用标准宏值，或统一采用 POSIX 标准名并在 UAPI 边界保持一致（返回值统一为 `-ERRNO`）。

---

## 4. 微内核核心与多架构 (`arm`, `arm64`, `riscv`, `x86_64`) 设计契约

### 4.1 微内核最小对象集（7 类核心内核对象）
微内核本体控制在 8,000~12,000 行 C 代码以内，仅管理以下 7 类通过 **代数句柄（Generation-counted Handle: `index:16 | gen:16`）** 引用的内核对象：
1. **`Task / Thread`**：调度实体（TCB），包含寄存器上下文、优先级、时间片、IPC 阻塞状态、TLS 指针。
2. **`Process / VmSpace`**：资源容器，持有地址空间（MMU 页表或 MPU Region 表）与进程私有 Capability 表 (`cap_table`)。
3. **`Endpoint`**：同步 RPC 通道（`IPC_CALL` / `IPC_RECV` / `IPC_REPLY_AND_WAIT`），内置调用方到服务方的**优先级继承（Priority Inheritance）**与防伪造 `token`。
4. **`Notification`**：轻量级异步事件位图（类似 seL4 Notification / Linux eventfd），供中断下半部、定时器、异步唤醒使用。
5. **`Mutex / Futex`**：支持优先级天花板/优先级继承与 Owner 崩溃恢复（`EOWNERDEAD`）的同步原语。
6. **`IrqObject`**：将硬件中断号封装为受 Capability 保护的内核对象，硬件中断触发时自动 mask 并向绑定的 `Notification` 投递信号，唤醒用户态驱动协程/线程。
7. **`PhysMem / IoPort`**：封装物理内存段（MMIO / DMA 一致性内存）以及 x86 特有的 I/O 端口区间（`CAP_IOPORT`）。

### 4.2 固定大小 IPC 消息契约 (`struct nos_ipc_msg`)
为保证寄存器快速传参（Fastpath IPC）与跨组件二进制兼容，`include/uapi/nos/ipc.h` 定义的 IPC 消息头大小严格固定为 **64 字节**：
```c
struct nos_ipc_msg {
    uint32_t tag;           /* opcode(16) | flags(8) | cap_count(8) */
    uint32_t token;         /* 内核注入的客户端连接标识，服务端不可伪造 */
    int32_t  status;        /* 返回状态码 (0 或 -errno) */
    uint32_t payload_len;   /* 附加缓冲区字节数 */
    uint64_t regs[4];       /* 4 个 64-bit 内联寄存器消息槽（可传 32 字节短消息，无需内存拷贝） */
    uint64_t buf_addr;      /* 附加长消息缓冲区虚拟地址（配合 Grant/Map 或拷贝传输） */
    uint32_t caps[2];       /* 随消息传递的最多 2 个 Capability 句柄 */
};
_Static_assert(sizeof(struct nos_ipc_msg) == 64, "nos_ipc_msg ABI size must be 64 bytes");
```

### 4.3 多架构 HAL 6 文件标准契约 (`arch/<arch>/`)
严禁将多架构代码用 `#if defined(__arm__) ... #elif defined(__aarch64__)` 塞入同一个文件。每个架构目录 `arch/{arm,arm64,riscv,x86}/` 统一实现 `include/nos/hal_arch.h` 声明的接口，并按以下 6 个核心模块组织：

| 模块文件 | 职责说明 | `arm` (v7-M / v7-A) | `arm64` (v8-A) | `riscv` (RV32 / RV64) | `x86` (`x86_64`) |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `entry.S` | 启动入口、异常向量表、上下文保存与恢复、系统调用入口 | `Reset_Handler`, `SVC_Handler`, `PendSV_Handler` (v7-M) / Vector Table (v7-A) | `el1_vectors`, `el0_sync`, `eret` | `trap_vector`, `ecall`, `mret`/`sret` | Multiboot2/UEFI stub, `GDT`/`TSS`, `IDT`, `syscall`/`sysretq` |
| `trap.c` | 异常分发、Page/MPU Fault 处理、Syscall 分发桥接 | HardFault/MemManage/UsageFault/SVC 解析 | `ESR_EL1` / `FAR_EL1` 分发 | `scause` / `stval` 分发 | `#PF` (`CR2`), `#GP`, `#DF` 分发 |
| `mmu.c` 或 `mpu.c` | 实现统一的 `hal_vm_*` 接口 | ARMv7-M PMSAv7 (8/16 MPU Regions) 或 ARMv7-A Short-descriptor 页表 | 4KB 粒度 4 级页表 (`TTBR0_EL1` / `TTBR1_EL1`, ASID) | RV32 PMP 或 RV64 Sv39/Sv48 (`satp` + SFENCE.VMA) | 4 级页表 (`PML4` -> `PDPT` -> `PD` -> `PT`, `CR3` + PCID) |
| `irq.c` | 中断控制器驱动与掩码控制 | NVIC (v7-M) / GICv2 (v7-A) | GICv2 / GICv3 | CLINT + PLIC / AIA | Local APIC (xAPIC/x2APIC) + IOAPIC + MSI/MSI-X |
| `timer.c` | 系统滴答与高精度单次定时 (Tickless) | SysTick / Generic Timer | ARM Generic Timer (`CNTP_TVAL_EL0`) | RISC-V Timer (`mtimecmp` / SBI Timer) | LAPIC Timer + TSC-Deadline / HPET |
| `smp.c` | 多核启动、Per-CPU 指针寄存器、核间中断 (IPI) | 单核或 PSCI / SGI | PSCI 启动从核，`TPIDR_EL1` 存 Per-CPU | SBI HSM 启动从核，`tp` 存 Per-CPU，Software IPI | INIT-SIPI-SIPI 启动 AP，`GS.base` (`MSR_KERNEL_GS_BASE`) 存 Per-CPU |

### 4.4 双模地址空间抽象 (`struct vm_space`)
为同时支持带 MMU 的处理器（`arm64`, `riscv64`, `x86_64`, `cortex-a7`）与仅带 MPU/PMP 的微控制器（`cortex-m3/m4/m7`, `rv32`），`kernel/mm/` 提供统一的 `vm_space` 接口：
- **`CONFIG_MMU=y` (`kernel/mm/vmm_mmu.c`)**：每个用户进程拥有独立虚拟地址空间，支持 `fork` (Copy-on-Write)、按需分页、`mmap` 任意虚拟地址映射。
- **`CONFIG_MPU=y` 或 No-MMU (`kernel/mm/vmm_mpu.c`)**：采用单地址空间（SASOS）固定槽位分区（Fixed-slot MPU Regions：如 Flash Text 只读、Process Private SRAM 读写、Shared IPC Buffer、授权外设 MMIO 窗口），任务切换时仅需改写 4~8 个 MPU 寄存器（约十几个时钟周期），并提供 `posix_spawn` + `vfork/exec` 语义。

---

## 5. CMake + Ninja + Kconfiglib 构建系统规范

### 5.1 为什么彻底切换至 CMake + Ninja + Kconfiglib？
1. **根除手写正则 Kconfig 解析器的 Bug**：仓库自带完整的 `scripts/kconfiglib.py`（Ulf Magnusson 标准实现），只需修复 `arch/Kconfig` 中的非标准 `source "..." if ...` 语法，即可获得 100% 准确的 `depends on`、`select`、`choice`、`if/endif` 解析能力。
2. **一等公民的编译目标（Target）隔离**：
   - 在旧 Makefile 中，所有目录共享全局 `CCFLAGS` 和 `INC_DIR`。
   - 在 CMake 中，`nos_kernel`（`-ffreestanding -nostdlib`）、`nos_soc`、`nos_board`、`nos_drivers`、`nos_fs`、`nos_app` 各自作为独立 Object Library Target，通过 `target_include_directories` 与 `target_compile_options` 精确控制头文件可见性与编译宏。
   - 微内核演进时，用户态服务只需调用 `nos_add_user_service(vfs_server ...)`，CMake 自动为其切换用户态编译标志或调用 `objcopy --prefix-sections=.user` 注入用户态段，无需脆弱的链接脚本路径通配符。
3. **跨架构工具链彻底解耦**：切换架构只需切换 `-DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/<arch>.cmake` 与 `-DNOS_DEFCONFIG=configs/<board>_defconfig`。

### 5.2 构建数据流图
```text
configs/<board>_defconfig  +  Kconfig 树 (Kconfig, arch/Kconfig, soc/Kconfig, board/Kconfig, drivers/Kconfig, fs/Kconfig)
                                    │
                                    ▼  (scripts/gen_kconfig_cmake.py 调用 scripts/kconfiglib.py)
        ┌───────────────────────────┼───────────────────────────┐
        ▼                           ▼                           ▼
  ${BINARY_DIR}/.config    ${BINARY_DIR}/include/autocfg.h   ${BINARY_DIR}/kconfig.cmake
                           ${BINARY_DIR}/include/autoconf.h  (导出 set(CONFIG_XXX ...) 变量)
                                    │
                                    ▼
                        CMakeLists.txt (生成 build.ninja)
                                    │
                                    ▼
        ┌───────────────────────────┴───────────────────────────┐
        ▼                                                       ▼
  Pass 1: 链接 nos_pass1.elf (含空 kallsyms_table)        预处理 nos_kernel.ld.S -> nos_kernel.ld
        │
        ▼  (objdump -t | scripts/mkkallsyms.py)
  Pass 2: 编译真实 kallsyms_table.c -> 链接最终 nos.elf
        │
        ▼
  导出产物: nos.elf / nos.bin / nos.hex / nos.img / nos.list / nos.map
```

### 5.3 常用构建命令速查
整个项目采用 **100% 纯 CMake + Ninja** 构建体系（已彻底移除所有 `Makefile` 和 Unix Shell 依赖），在 **Windows (PowerShell / CMD / VS Code CMake Tools)**、**macOS** 和 **Linux** 下体验完全一致：

```bash
# 方式 A：使用 CMakePresets（推荐，Windows / macOS / Linux 通用，支持 VS Code / CLion 一键选择）
cmake --preset nk60-v2
cmake --build --preset nk60-v2

cmake --preset qemu-stm32
cmake --build --preset qemu-stm32

# 方式 B：手动指定构建目录、工具链与 defconfig（适合自定义板型或叠加配置片段）
cmake -B build/nk60-v2 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/arm-none-eabi.cmake \
  -DNOS_DEFCONFIG=configs/nk60-v2_defconfig
ninja -C build/nk60-v2
```

---

## 6. 四阶段平滑演进路线图 (Migration Roadmap)

- **阶段 1（当前阶段：基础规范、Kconfig 修复、冲突治理与 CMake 首版落地）：**
  1. 输出本设计规范文档（`docs/ARCHITECTURE_AND_BUILD_SPEC.md`）。
  2. 修复 `arch/Kconfig` 语法错误，补全 `soc/Kconfig`、`board/Kconfig`、`drivers/Kconfig`、`Kconfig` 中缺失的配置项及 `riscv`/`x86` 架构扩展点。
  3. 编写 `scripts/gen_kconfig_cmake.py`（基于 `scripts/kconfiglib.py`），同时修复 Makefile 中的 `%_config` 调用，使旧 Makefile 与新 CMake 都能生成完全正确的 `.config` 与 `include/autocfg.h`。
  4. 修复 `arch/arm/include/asm/types.h`（使用 `<stdint.h>` 定宽类型消除与 Newlib `int32_t` 的冲突）与 `include/kernel/compiler-gcc.h`（增加 `#ifndef` 保护消除与 `<sys/cdefs.h>` 的宏重定义）。
  5. 落地第一版 `CMakeLists.txt` 与 `cmake/` 工具链模块，确保 `nk60-v2` 与 `qemu-stm32` 在 CMake + Ninja 下 100% 编译链接通过并生成 `.elf`/`.bin`/`.hex`/`.img`。

- **阶段 2（头文件净化与标准库端口层落地）：**
  1. 建立 `include/uapi/nos/{types.h,errno.h,syscall.h,ipc.h,cap.h,abi.h}` 与 `include/nos/klibc.h`。
  2. 迁移 `codex/nos/libc/port/newlib.c` 为 `libc/port/picolibc_newlib.c`，接管 `_reent`、`malloc_lock` 与 VFS `_read/_write/_open/_close`，随后删除顶层伪造的 `include/stdio.h`。
  3. 清理 `board/{nk60-v2,qemu-stm32,wl-001,zj-v3}/drivers` 中与 `soc/stm32f1`、`soc/stm32f4` 重复的 SPL 外设库源码。

- **阶段 3（微内核核心机制与用户态服务抽取）：**
  1. 从 `codex/nos` 提炼干净的 `kernel/core/handle.c`、`kernel/cap/cap_table.c`、`kernel/ipc/endpoint.c`（固定 64B `nos_ipc_msg`，零板级 `#ifdef`）与 `kernel/mm/{vmm_mmu.c,vmm_mpu.c}`。
  2. 拆分 `servers/vfs/`、`servers/procmgr/` 与 `user/libnos/`、`user/posix/`。

- **阶段 4（多架构 `arm64` / `riscv64` / `x86_64` 与 musl POSIX 完整闭环）：**
  1. 按 6 文件契约补齐 `arch/arm64/`、`arch/riscv/`、`arch/x86/` 在 QEMU `virt` / `q35` 上的引导、页表、中断、定时器与上下文切换。
  2. 接入 `musl` libc 与 `user/posix/` 测试集，在 QEMU 四架构下通过自动化冒烟测试。
