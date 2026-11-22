# NOS ARM64 AI-Native 混合内核 Stage 0~1 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 NOS 补齐工业级 ARM64（ARMv8.2+/ARMv9）混合内核核心底座与 AI 时代 UAPI 契约，并打通 `qemu-virt-arm64` 从 EL2→EL1 引导、Higher-Half MMU、GICv3/v2、Generic Timer、上下文切换、PL011 中断串口到 `nShell` 的完整构建与测试闭环（对应设计规范 Stage 0~1）。

**Architecture:** 采用 Capability-Native 混合内核架构（TTBR1_EL1 Higher-Half 内核空间 + TTBR0_EL1 用户/Agent Cell 隔离空间），将构建体系与 64 位 MM 前置修复作为 Stage 0，随后按模块契约落地 `arch/arm64/{boot,cpu,kernel,mm,hal}`、`drivers/serial/uart_pl011.c` 及 `board/qemu-virt/`，同时以独立主机单元测试验证 64 位页表、定时器防溢出与 AI UAPI 契约。

**Tech Stack:** C99/GNU11、AArch64 Assembly、CMake 3.20+ + Ninja + Kconfiglib、`aarch64-elf-gcc` (15.1.0)、`arm-none-eabi-gcc` (14.2.1)、Python Host Regression Suite。

## Global Constraints

- 所有对话、文档（spec/plan）与代码注释必须使用中文；文件头保持 Nick Hu 标准版权声明。
- 严格遵守 4 空格缩进（禁止 Tab）、Unix `\n` 行尾、K&R 大括号、`snake_case` 命名、`UPPER_SNAKE_CASE` 宏、显式 `NULL` 检查、返回 `0` 或负 errno。
- **严禁**在 `include/` 根目录创建 `string.h` / `stdio.h` / `stdlib.h` / `errno.h` 等标准 C 库同名头文件；freestanding 头仅限 `libs/libc/freestanding/include/`，且仅当工具链缺少 libc 时通过 `-isystem` 注入。
- 严禁在 `arch/arm64/` 或 `kernel/` 中硬编码板级地址或 `#include` 板级头文件（`python3 scripts/check_layer_deps.py` 必须通过）。
- `arch/arm64/compiler_flags.cmake` 在 `CONFIG_ARM64_CORTEX_A53` 下必须保持 `NOS_ARCH_FLAGS` 严格等于 `-march=armv8-a;-mcpu=cortex-a53`（确保 `tests/test_arch_flags.cmake` 通过）。
- 全部既有 Cortex-M 预设（`nk60-v2`、`qemu-stm32`、`wl-001`、`zj-v3`）与既有主机回归测试必须保持 100% 绿灯。

---

## File Structure Map

- **设计规范与构建适配（Stage 0）：**
  - `docs/superpowers/specs/2026-11-22-arm64-ai-native-kernel-design.md`
  - `cmake/libc_detect.cmake`, `CMakeLists.txt`, `cmake/firmware.cmake`, `libs/libc/CMakeLists.txt`
  - `libs/libc/freestanding/include/{string.h,stdio.h,stdlib.h,ctype.h,errno.h,inttypes.h,unistd.h,fcntl.h,sys/types.h,sys/stat.h}`
  - `libs/libc/freestanding/klibc_runtime.c`
  - `libs/vsprintf.c`, `include/kernel/mm.h`, `kernel/mm/mm_node.c`, `kernel/task/task_dump.c`, `scripts/mkkallsyms.py`
  - `include/uapi/nos/{syscall.h,cap.h,ai.h}`, `kernel/syscall/syscall.c`
- **ARM64 架构底座（Stage 1）：**
  - `arch/arm64/Kconfig`, `arch/arm64/CMakeLists.txt`, `arch/arm64/compiler_flags.cmake`, `arch/arm64/init.c`
  - `arch/arm64/include/asm/{sysreg.h,pt_regs.h,cpufeature.h,percpu.h,psci.h,gic.h,mmu.h,irq.h,spinlock.h,switch.h,platform.h}`
  - `arch/arm64/boot/start.S`
  - `arch/arm64/mm/{mmu.c,fixmap.c}`
  - `arch/arm64/cpu/{cpufeature.c,psci.c,fpu.c,cpu.c}`
  - `arch/arm64/kernel/{vectors.S,trap.c,switch.S,task.c,irq.c,timer.c,atomic.c,percpu.c}`
  - `arch/arm64/hal/hal_arm64.c`
- **驱动与板级适配（Stage 1）：**
  - `drivers/serial/uart_pl011.c`
  - `board/qemu-virt/{nos_kernel.ld.S,board_mm.c,init.c,sources.cmake,board.dts}`
  - `configs/qemu-virt-arm64_defconfig`
  - `cmake/utility_targets.cmake`
- **测试与验证：**
  - `tests/host/{test_arm64_mmu.c,run_arm64_mmu_test.py,test_arm64_gic_timer.c,run_arm64_gic_timer_test.py}`
  - `tests/run_host_regressions.py`
  - `tests/qemu_arm64_smoke.py`

---

### Task 1: 工具链 Libc 探测与隔离式 Freestanding Libc 层（Stage 0）

**Files:**
- Create: `cmake/libc_detect.cmake`
- Create: `libs/libc/freestanding/include/{string.h,stdio.h,stdlib.h,ctype.h,errno.h,inttypes.h,unistd.h,fcntl.h,sys/types.h,sys/stat.h}`
- Create: `libs/libc/freestanding/klibc_runtime.c`
- Modify: `CMakeLists.txt`, `cmake/firmware.cmake`, `libs/libc/CMakeLists.txt`, `libs/vsprintf.c`

**Interfaces:**
- Produces: CMake 变量 `NOS_HAS_TOOLCHAIN_LIBC`（`TRUE`/`FALSE`），freestanding 标准头声明与内核态 `malloc`/`free`/`strtol`/`qsort`/`printf`/`__nos_errno_location` 实现。

- [x] **Step 1:** 编写 `cmake/libc_detect.cmake`，使用 `${CMAKE_C_COMPILER}` 预处理测试 `#include <string.h>` 并设定 `NOS_HAS_TOOLCHAIN_LIBC`；若为 `FALSE`，向 `nos_headers` 追加 `-isystem ${CMAKE_SOURCE_DIR}/libs/libc/freestanding/include`。
- [x] **Step 2:** 更新 `CMakeLists.txt`、`cmake/firmware.cmake`（无工具链 libc 时仅链接 `-lgcc` 并附加 `-nostdlib`）和 `libs/libc/CMakeLists.txt`（切换 `port/picolibc_newlib.c` 与 `freestanding/klibc_runtime.c`）。
- [x] **Step 3:** 实现 `libs/libc/freestanding/include/` 下最小标准头（注意 `errno.h` 防循环包含保护，兼容 `include/uapi/nos/errno.h` 中的 `#if __has_include(<errno.h>)`）及 `klibc_runtime.c`。
- [x] **Step 4:** 修改 `libs/vsprintf.c`，在 `defined(__aarch64__) && !defined(__ARM_FP)` 下安全裁剪浮点 `%f` 分支，确保 `-mgeneral-regs-only` 下零错误。
- [x] **Step 5:** 验证：跑 `cmake --preset qemu-stm32 && cmake --build --preset qemu-stm32` 及 `python3 tests/host/run_formatter_test.py` 确认现有 Cortex-M 与格式化器测试无回归。

---

### Task 2: 64 位内核前置修复、编译标志与 AI 原生 UAPI 契约（Stage 0）

**Files:**
- Modify: `include/kernel/mm.h`, `kernel/mm/mm_node.c`, `kernel/task/task_dump.c`, `scripts/mkkallsyms.py`
- Modify: `arch/arm64/Kconfig`, `arch/arm64/compiler_flags.cmake`
- Modify: `include/uapi/nos/{syscall.h,cap.h}`, `kernel/syscall/syscall.c`
- Create: `include/uapi/nos/ai.h`

**Interfaces:**
- Consumes: `include/uapi/nos/{types.h,errno.h,ipc.h}`
- Produces: 64 位安全 `struct mm_node` / `struct page`（`pfn` 为 `addr_t`）、`include/uapi/nos/ai.h`（`struct nos_ring_sqe` 64B、`struct nos_ring_cqe` 32B、`struct nos_audit_record` 64B、`struct nos_cell_budget`、`struct nos_vmo_create_args`、`struct nos_infer_sched_param`）。

- [x] **Step 1:** 将 `include/kernel/mm.h` 的 `start_pfn`/`pfn` 改为 `addr_t`，升级 `kernel/mm/mm_node.c` 计算变量为 `size_t`；调整 `scripts/mkkallsyms.py` 64 位地址 `UL` 后缀。
- [x] **Step 2:** 在 `kernel/task/task_dump.c` 增加 `__aarch64__` 下当前/阻塞任务 `x29(FP)/x30(LR)` 栈回溯分支。
- [x] **Step 3:** 完善 `arch/arm64/Kconfig` 与 `arch/arm64/compiler_flags.cmake`（将 `-mgeneral-regs-only` 放入 `NOS_FP_FLAGS`，保持 `NOS_ARCH_FLAGS` 不变）。
- [x] **Step 4:** 更新 `include/uapi/nos/syscall.h`（保留 `0..15`，新增 `16..23`）、`include/uapi/nos/cap.h`（新增 5 个 AI 能力类型与权限位），创建 `include/uapi/nos/ai.h` 并在 `kernel/syscall/syscall.c` 对未实现 AI 系统调用预留 `-ENOSYS` 占位分发表。
- [x] **Step 5:** 验证：`cmake -P tests/test_arch_flags.cmake` 与 `python3 tests/host/run_mm_buddy_test.py`、`python3 tests/host/run_mm_vmm_test.py` 全部 PASS。

---

### Task 3: ARM64 体系结构头文件与真实 Stage-1 硬件 MMU（Stage 1）

**Files:**
- Create: `arch/arm64/include/asm/{sysreg.h,pt_regs.h,cpufeature.h,percpu.h,psci.h,gic.h}`
- Modify: `arch/arm64/include/asm/{mmu.h,irq.h,spinlock.h,switch.h,platform.h}`
- Create: `arch/arm64/mm/{mmu.c,fixmap.c}`
- Create: `tests/host/{test_arm64_mmu.c,run_arm64_mmu_test.py}`

**Interfaces:**
- Produces: `arm64_pte_make_leaf()`, `arm64_pt_map_range()`, `arm64_pt_unmap_range()`, `arm64_pt_mprotect()`, `arm64_pt_translate()`, `arm64_asid_alloc()`, `arm64_mmu_init_kernel_wx()`, `arm64_ioremap()`, `arm64_virt_to_phys()`, `arm64_phys_to_virt()`.

- [x] **Step 1:** 编写 `tests/host/test_arm64_mmu.c` 与 `tests/host/run_arm64_mmu_test.py`（覆盖 39/48-bit 映射、2MB Block、64KB `PTE_CONT`、W^X 拒绝、mprotect、ASID 世代回绕），运行确认因缺 `mmu.c` 而失败。
- [x] **Step 2:** 创建与补齐 `arch/arm64/include/asm/*.h` 全部系统寄存器、异常帧（288B `_Static_assert`）、MMU（Higher-Half 基址与 ARMv8 PTE 描述符宏）、Per-CPU、GIC 与 PSCI 契约。
- [x] **Step 3:** 实现 `arch/arm64/mm/mmu.c` 与 `fixmap.c`（严格 BBM 与 TLBI，主机单测通过 `#if defined(__aarch64__)` 安全编译测试）。
- [x] **Step 4:** 运行 `python3 tests/host/run_arm64_mmu_test.py`，验证全部用例 PASS。

---

### Task 4: EL2→EL1 启动、异常向量、CPU 特性探测、PSCI 与惰性 FP（Stage 1）

**Files:**
- Create: `arch/arm64/boot/start.S`
- Create: `arch/arm64/kernel/{vectors.S,trap.c,percpu.c,atomic.c}`
- Create: `arch/arm64/cpu/{cpufeature.c,psci.c,fpu.c,cpu.c}`

**Interfaces:**
- Consumes: `arm64_mmu_init_kernel_wx()`, `nos_start()`, `nos_syscall_dispatch()`, `arm64_irq_dispatch()`, `arm64_irq_exit_resched()`.
- Produces: `_start`, `arm64_vectors`, `nos_arm64_boot()`, `arm64_cpufeature_init()`, `psci_system_reset()`, `arm64_fpu_trap_handler()`, `arch_dump_stack()`.

- [x] **Step 1:** 编写 `arch/arm64/boot/start.S`，完成 EL2→EL1 降级、BSS 清零、临时 `TTBR0` + 正式 `TTBR1` 构建、开 MMU 与跳转高半核 `nos_arm64_boot`。
- [x] **Step 2:** 编写 `arch/arm64/kernel/vectors.S` 与 `trap.c`，实现 2KB 对齐 16 槽向量表、288B `pt_regs`、`SVC #0` 分发、FP 惰性陷阱处理与精确异常栈回溯。
- [x] **Step 3:** 实现 `arch/arm64/cpu/{cpufeature.c,psci.c,fpu.c,cpu.c}` 及 `kernel/{percpu.c,atomic.c}`，完成特性探测、PSCI HVC/SMC 复位/电源管理、Per-CPU 初始化与 64 位原子操作。

---

### Task 5: GICv3/v2 中断、Generic Timer、上下文切换与 HAL 闭环（Stage 1）

**Files:**
- Create: `arch/arm64/kernel/{irq.c,timer.c,switch.S,task.c}`
- Modify: `arch/arm64/hal/hal_arm64.c`, `arch/arm64/init.c`, `arch/arm64/CMakeLists.txt`
- Create: `tests/host/{test_arm64_gic_timer.c,run_arm64_gic_timer_test.py}`
- Modify: `tests/run_host_regressions.py`

**Interfaces:**
- Produces: `arm64_gic_init()`, `nos_irq_register()`, `arm64_irq_dispatch()`, `arm64_timer_ticks_to_ns()`, `context_switch()`, `context_switch_to()`, `stack_init()`, 完整 `hal_*` 契约实现。

- [x] **Step 1:** 编写 `tests/host/test_arm64_gic_timer.c` 与 `run_arm64_gic_timer_test.py`（验证 128-bit 纳秒换算防溢出、GIC 中断表注册/分发/越界检查、AI UAPI 结构体 `_Static_assert` 尺寸），运行确认失败。
- [x] **Step 2:** 实现 `arch/arm64/kernel/irq.c`（GICv3 SysReg + GICv2 自动回退与动态 IRQ 表）和 `arch/arm64/kernel/timer.c`（128-bit 换算 + `CNTP_CVAL_EL0` 无漂移周期中断 + Oneshot）。
- [x] **Step 3:** 实现 `arch/arm64/kernel/switch.S` 与 `task.c`，严格落实“双路径同步/中断延迟切换 + `saved_irq_flags` 栈帧交接 + `arm64_finish_first_switch()` 释放首次切入 `rq->lock` 与开中断”数学闭环。
- [x] **Step 4:** 更新 `arch/arm64/hal/hal_arm64.c`、`arch/arm64/init.c` 与 `arch/arm64/CMakeLists.txt`。
- [x] **Step 5:** 运行 `python3 tests/host/run_arm64_gic_timer_test.py` 并将其和 `run_arm64_mmu_test.py` 接入 `tests/run_host_regressions.py`。

---

### Task 6: PL011 中断串口、QEMU Virt 板级闭环与全矩阵验证（Stage 1）

**Files:**
- Modify: `drivers/serial/uart_pl011.c`, `cmake/utility_targets.cmake`, `configs/qemu-virt-arm64_defconfig`, `CMakePresets.json`
- Create: `board/qemu-virt/nos_kernel.ld.S`, `board/qemu-virt/board_mm.c`
- Modify: `board/qemu-virt/{init.c,sources.cmake,board.dts}`
- Create: `tests/qemu_arm64_smoke.py`

**Interfaces:**
- Consumes: `arm64_ioremap()`, `nos_irq_register()`, `arm64_gic_init()`, `hal_irq_enable()`.
- Produces: 可直接运行的 `build/qemu-virt-arm64/nos.elf`、`nos.bin`、`nos.hex`、`nos.img` 及 `qemu-run` / `qemu-run-gdb` 目标与 `tests/qemu_arm64_smoke.py`。

- [x] **Step 1:** 升级 `drivers/serial/uart_pl011.c`：ARM64 下采用 `arm64_ioremap` 基址并启用 `kfifo` RX + GIC SPI 33 中断阻塞读取，同时完全保持主机单测 `run_pl011_uart_test.py` 兼容。
- [x] **Step 2:** 创建 `board/qemu-virt/nos_kernel.ld.S`（双段 Higher-Half + 严格 W^X 页对齐 + 紧凑 `nos.bin`）、`board/qemu-virt/board_mm.c`，更新 `init.c` / `sources.cmake` / `board.dts` / `configs/qemu-virt-arm64_defconfig` / `cmake/utility_targets.cmake` / `CMakePresets.json`。
- [x] **Step 3:** 创建 `tests/qemu_arm64_smoke.py` 冒烟测试脚本。
- [x] **Step 4:** 执行全矩阵验证（详见执行输出：`qemu-virt-arm64` 构建与 `readelf` 核验通过、`nk60-v2`/`qemu-stm32`/`wl-001`/`zj-v3` 全通过、`test_arch_flags.cmake`/`test_soc_headers.cmake`/`check_layer_deps.py` 全通过、新增 `run_arm64_mmu_test.py` 与 `run_arm64_gic_timer_test.py` 全通过；本机未安装 `qemu-system-aarch64`，`qemu_arm64_smoke.py` 退出码 2 跳过）。

---

## Stage 2 ~ Stage 4 后续迭代索引（在 Stage 0~1 验收通过后接续立项）

- **Stage 2 Plan（后续）**：PSCI SMP 多核上线、`SVC #0` EL0 用户态与 musl 运行时、CDT 可撤销能力树、`Agent Cell`（预算/审计）+ `VMO` 首版。
- **Stage 3 Plan（后续）**：`RingIPC` 无锁 SQ/CQ + Schema 白名单、`VMO_KVCACHE` OS 级页表分页 + CoW 前缀共享、`Cell` Snapshot/Rollback/Fork、`sched_infer` EDF/CBS + 惰性 SVE/SME + EAS + Gang 调度。
- **Stage 4 Plan（后续）**：`SMMUv3` + `kernel/accel/`（QEMU `virtio-accel` + RK3588 `rknpu`）、ARMv9 PAC/BTI/MTE 与 CCA Realm。
