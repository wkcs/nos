# NOS ARM64 AI-Native 混合内核 Stage 2 实施计划（SMP + EL0 + CDT/VMO/Agent Cell）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 Stage 0~1 已跑通单核 `qemu-virt-arm64`（Higher-Half MMU + GICv3/v2 + Generic Timer + PL011 中断 + `nShell`）基础上，完成 ARM64 Stage 2 全部能力：
- **Stage 2A**：打通多核 Per-CPU 内核基础设施与 4 核 PSCI SMP 上线/跨核负载均衡，落地真实 `EL0` 用户态降级、独立 `TTBR0_EL1` + 16-bit ASID、`SVC #0` 系统调用路径与硬件写保护/越界 Data Abort 隔离闭环，并在真实 4 核 `qemu-system-aarch64` 下实测验证；
- **Stage 2B**：落地双层基数 `CNode` + `CDT` 可级联撤销能力树、统一零拷贝 `VMO / TensorObj` 内存对象、以及 `Agent Cell` 五维硬预算 + EL0 只读 Telemetry 页 + 64 字节强制审计日志。

**Architecture:** 沿用《[NOS ARM64 AI-Native 混合内核架构设计与演进规范](../specs/2026-11-22-arm64-ai-native-kernel-design.md)》中已评审确认的 **Capability-Native 混合内核（方案 A）**：内核核心仲裁与热路径（`CDT`、`VMO`、`Agent Cell`、GICv3/Timer、PSCI SMP、`sched`）驻留 EL1 Higher-Half（`TTBR1_EL1`）；不可信业务与未来 Agent 沙箱运行在独立 `TTBR0_EL1` + `ASID` 的 EL0 态，零默认权限，经 `SVC #0` 与指针合法性防线受控访问系统能力。所有架构扩展通过 `CONFIG_NR_CPUS` / `CONFIG_ARCH_ARM64` 优雅隔离开销，保持现有 Cortex-M（`nk60-v2`、`qemu-stm32`、`wl-001`、`zj-v3`）100% 零回归。

**Tech Stack:** C99/GNU11、AArch64 Assembly、CMake 3.20+ + Ninja + Kconfiglib、`aarch64-elf-gcc` (15.1.0)、`arm-none-eabi-gcc` (14.2.1)、`qemu-system-aarch64` (11.1.3)、`qemu-system-arm`、Python Host Regression Suite。

---

## Global Constraints

- 所有对话、文档与代码注释必须使用中文；文件头保持 Nick Hu 标准版权声明。
- 严格遵守 4 空格缩进（禁止 Tab；`Kconfig` 文件除外）、Unix `\n` 行尾、K&R 大括号、`snake_case` 命名、`UPPER_SNAKE_CASE` 宏、显式 `NULL` 检查、返回 `0` 或负 errno。
- 严格遵循层级依赖与编译边界：`init/` 与 `kernel/` 严禁直接 `#include <asm/...>` 或板级头文件（`python3 scripts/check_layer_deps.py` 必须通过）。
- 高半核 C 代码访问 `.text.boot` / `.bss.boot` 低半核引导符号必须通过 `.rodata` 的 64 位 `.quad` 物理地址指针，严禁触发 `-mcmodel=small` 跨 4GB PC 相对重定位溢出。
- 跨核及自修改代码区域必须正确维护 Cache（`dc cvac` 刷至 PoC，`dc cvau + ic ivau` 同步指令缓存）与 TLB（`tlbi aside1is` / `tlbi vmalle1is` + `dsb ish` + `isb`）。
- 全部既有 Cortex-M 预设（`nk60-v2`、`qemu-stm32`、`wl-001`、`zj-v3`）与相关主机/QEMU 回归测试必须保持 100% 通过。

---

## 阶段划分总览

- **Stage 2A（SMP + 真实 EL0 + `SVC #0` 硬件隔离闭环）**：Task 1 ~ Task 3
- **Stage 2B（CDT 能力树 + VMO + Agent Cell）**：Task 4 ~ Task 6

---

## Stage 2A：多核 PSCI SMP 与真实 EL0 硬件隔离

### Task 1: Per-CPU 内核并发基础设施与 4 核 PSCI SMP 启动

**Files:**
- Modify: `include/kernel/sched.h`, `kernel/sched/sched.c`
- Modify: `include/kernel/irq.h`, `kernel/irq/irq.c`
- Modify: `kernel/task/task.c`, `init/init.c`
- Modify: `arch/arm64/include/asm/mmu.h`, `arch/arm64/mm/mmu.c`
- Modify: `arch/arm64/boot/start.S`, `arch/arm64/kernel/timer.c`, `arch/arm64/kernel/irq.c`, `arch/arm64/hal/hal_arm64.c`
- Create: `arch/arm64/cpu/smp.c`
- Modify: `arch/arm64/CMakeLists.txt`, `arch/arm64/Kconfig`
- Modify: `board/qemu-virt/board.dts`, `configs/qemu-virt-arm64_defconfig`, `cmake/utility_targets.cmake`

**Interfaces:**
- Consumes: `psci_cpu_on()`, `arm64_percpu_init()`, `arm64_gic_cpu_init()`, `arm64_gic_send_sgi()`, `hal_cpu_id()`.
- Produces: Per-CPU `scheduler_lock_nest` 与 `irq_nest_count()`、跨核安全的 `clean_close_task()`、`arm64_secondary_entry`、`arm64_secondary_main()`、`arch_smp_boot_secondary_cpus()`、`arm64_smp_online_count()`、`arm64_mmu_get_kernel_ttbr1()`、`arm64_clean_dcache_poc()`、`arm64_sync_icache_dcache()`、`arm64_tlbi_asid()`。

- [x] **Step 1:** 在 `include/kernel/sched.h` 与 `kernel/sched/sched.c` 实现 `CONFIG_NR_CPUS > 1` 下的 Per-CPU `g_scheduler_lock_nest[CONFIG_NR_CPUS]` 宏重定向 `scheduler_lock_nest`，并将 `calculate_sys_cycle()` 限制为仅 CPU 0 推进全局秒级节拍；在 `include/kernel/irq.h`、`kernel/irq/irq.c` 与 `arch/arm64/hal/hal_arm64.c` 实现 Per-CPU `g_interrupt_nest[CONFIG_NR_CPUS]` 与 `irq_nest_count()`。
- [x] **Step 2:** 在 `kernel/task/task.c` (`clean_close_task()`) 中跳过 `on_cpu != 0` 的待回收任务，并在 `CONFIG_NR_CPUS > 1` 下通过目标核 `rq->lock` 内存屏障保证先切出旧任务栈再执行 `task_free()`；在 `init/init.c` 的 `idel_task_init()` 中将从核 `rq->online` 默认置 `false`，并在 `core_task_entry()` 优先调用弱符号 `arch_smp_boot_secondary_cpus()`。
- [x] **Step 3:** 在 `arch/arm64/include/asm/mmu.h` 与 `arch/arm64/mm/mmu.c` 实现 Cache/TLB 维护接口（`arm64_clean_dcache_poc`、`arm64_sync_icache_dcache`、`arm64_tlbi_asid`、`arm64_mmu_get_kernel_ttbr1`、`ARM64_USER_VA_LIMIT`）。
- [x] **Step 4:** 扩充 `arch/arm64/boot/start.S`（`arm64_secondary_entry`、EL2→EL1 降级、装入早期 `TTBR0` + 共享 `TTBR1`、切至从核栈并跳 `arm64_secondary_main`、导出 `.rodata` 64 位物理地址指针），升级 `arch/arm64/kernel/timer.c` 的 Per-CPU `g_next_cval[CONFIG_NR_CPUS]` 及 `arm64_timer_init_secondary()`，升级 `arch/arm64/kernel/irq.c` 的 `GICR_TYPER` Affinity 遍历、`NOS_IPI_RESCHED`（SGI 1）使能及 `ICC_SGI1R_EL1` 多集群 MPIDR 编码。
- [x] **Step 5:** 新建 `arch/arm64/cpu/smp.c` 完成 4 核从核 idle 任务绑定、`psci_cpu_on()` 上线、同步等待及 SGI 重调度句柄注册；更新 `board/qemu-virt/board.dts`、`configs/qemu-virt-arm64_defconfig`（`CONFIG_SMP=y`, `CONFIG_NR_CPUS=4`）和 `cmake/utility_targets.cmake`（`-smp 4`）。
- [x] **Step 6:** 验证：`cmake --preset qemu-virt-arm64 && cmake --build --preset qemu-virt-arm64` 以及 `python3 tests/host/run_sched_multi_policy_test.py` 全部 PASS。

---

### Task 2: 真实 EL0 用户态降级、`SVC #0` 指针防线与硬件 Fault 隔离

**Files:**
- Modify: `arch/arm64/hal/hal_arm64.c`, `arch/arm64/include/asm/switch.h`
- Modify: `arch/arm64/kernel/vectors.S`, `arch/arm64/kernel/trap.c`
- Modify: `arch/arm64/Kconfig`, `arch/arm64/CMakeLists.txt`

**Interfaces:**
- Produces: `arm64_enter_user_mode(user_pc, user_sp, arg0)`、`arm64_user_ptr_valid(ptr, len)`、`arm64_set_el0_fault_observer(...)`。

- [x] **Step 1:** 更新 `arch/arm64/hal/hal_arm64.c`：`hal_vm_switch(NULL)` 显式清零 `TTBR0_EL1` 防止内核/空页表线程误保留前一个用户空间映射；`hal_vm_space_destroy()` 清理页表前广播 `arm64_tlbi_asid(space->asid)`。
- [x] **Step 2:** 在 `arch/arm64/kernel/vectors.S` 实现 `arm64_enter_user_mode(user_pc, user_sp, arg0)`：屏蔽异常并设置 `ELR_EL1 = user_pc`、`SP_EL0 = user_sp`、`SPSR_EL1 = PSR_MODE_EL0t`、清零临时寄存器后 `eret` 落入硬件 EL0。
- [x] **Step 3:** 在 `arch/arm64/kernel/trap.c` 实现 `arm64_user_ptr_valid()`（拒绝 `< PAGE_SIZE` 及 `>= ARM64_USER_VA_LIMIT` 含内核空间地址防伪校验）、在 `EC_SVC64` 打开本地 IRQ 并发响应后校验 `SYS_NOS_DEBUG_CONSOLE` / `SYS_NOS_CLOCK_GET` 用户指针（非法直接返回 `-EFAULT`）；对来自 EL0 的 Data/Instr Abort/权限错误等同步 Fault 打印 `[EL0-FAULT]` 告警、回调观察器、并安全 `task_del(current)` 终止故障用户任务而不触发内核 `panic()`。

---

### Task 3: Stage 2A 原生 AArch64 EL0 硬件自测与全矩阵 QEMU 实测

**Files:**
- Create: `arch/arm64/kernel/el0_smoke.S`, `arch/arm64/kernel/stage2_selftest.c`
- Modify: `tests/qemu_arm64_smoke.py`

**Interfaces:**
- Produces: `arm64_el0_smoke_code_start..end`、`arm64_el0_fault_code_start..end`、`ARM64 Stage2A selftest PASS`。

- [x] **Step 1:** 编写位置无关原生汇编测试载荷 `arch/arm64/kernel/el0_smoke.S`：验证 EL0 用户栈读写、`SVC #0` 调用 `SYS_NOS_CLOCK_GET`、故意以高半核内核指针调用 `SYS_NOS_DEBUG_CONSOLE` 校验返回 `-EFAULT`、打印 `[EL0] Hello via SVC #0!\n`、`SYS_NOS_EXIT` 退出；以及故意写入 `R--` 只读页触发 EL0 Data Abort 的 `arm64_el0_fault_code`。
- [x] **Step 2:** 编写 `arch/arm64/kernel/stage2_selftest.c`（由 `CONFIG_ARM64_STAGE2_SELFTEST` 控制并通过 `task_init_register` 自动运行）：依次完成 4 核 SMP 亲和性调度验证、独立 `TTBR0` + ASID + `R-X`/`R--`/`RW-` 真实 EL0 `SVC #0` 闭环验证（在 `task->cleanup` 中安全释放 3 个物理页），以及 EL0 只读页写入触发 Data Abort 隔离清理验证。
- [x] **Step 3:** 更新 `tests/qemu_arm64_smoke.py` 增加 `--smp` 参数（默认 `4`）与 `ARM64 SMP:`、`[EL0] Hello via SVC #0!`、`ARM64 Stage2A selftest PASS` 断言。
- [x] **Step 4:** 执行 Stage 2A 全矩阵验证：4 核 GICv3/A53、4 核 GICv2/A53、4 核 GICv3/A76、1 核 SMP 回退、4 个 Cortex-M 构建、STM32 QEMU 冒烟、`check_layer_deps.py` 与核心主机回归全部 PASS。

---

## Stage 2B：CDT 可撤销能力树 + VMO + Agent Cell（Stage 2A 实测通过后接续执行）

### Task 4: 双层基数 CNode 与 CDT 可撤销能力树（`SYS_NOS_CAP_REVOKE_TREE`）

**Files:**
- Modify: `include/nos/cap.h`, `kernel/cap/cap_table.c`, `kernel/syscall/syscall.c`
- Create: `tests/host/test_cdt_cap.c`, `tests/host/run_cdt_cap_test.py`

**Targets:**
- 保持 MCU 档（`CONFIG_NOS_CAP_MAX_SLOTS <= 16`）紧凑静态布局零回退；在 ARM64（或启用大 CNode 配置）上支持最多 4096 槽位按需页式扩展。
- 为每个 `nos_cap_slot` 增加侵入式 CDT 派生树指针（`parent / first_child / next_sibling / prev_sibling`），`nos_cap_derive` 严格单调衰减权限并挂载子节点。
- 实现 `nos_cap_revoke_tree()` 与 `SYS_NOS_CAP_REVOKE_TREE`（系统调用号 23）：原子 DFS 级联吊销子能力子树、递增 `gen` 失效全部旧句柄，并关联触发绑定 `Agent Cell` 的 `NOS_AUDIT_CAP_REVOKE` 审计项。
- 编写并跑通主机 TDD 单元测试 `run_cdt_cap_test.py`（验证多层派生、跨表授权、子树原子级联撤销及父指针断链）。

---

### Task 5: VMO / TensorObj 统一零拷贝内存对象（`SYS_NOS_VMO_CREATE` / `CONTROL`）

**Files:**
- Create: `include/nos/vmo.h`, `kernel/mm/vmo.c`
- Modify: `kernel/CMakeLists.txt`, `kernel/syscall/syscall.c`, `arch/arm64/mm/mmu.c`
- Create: `tests/host/test_vmo.c`, `tests/host/run_vmo_test.py`

**Targets:**
- 实现 `struct nos_vmo`（嵌入 `struct nos_object` 引用计数 + `enum nos_vmo_kind`: `ANON / WEIGHT / KVCACHE / DMA_TENSOR` + 物理页数组/CoW 共享页句柄 + 不可变封印标记 `sealed`）。
- 接入 `SYS_NOS_VMO_CREATE`（17）与 `SYS_NOS_VMO_CONTROL`（18）：支持创建、`NOS_VMO_OP_SEAL` 永久不可变封印（拒绝后续任何可写映射）、`NOS_VMO_OP_CLONE_COW` 写时复制克隆，以及将 VMO 映射到目标 `nos_vm_space`（支持 `NOS_CAP_RIGHT_MAP` 与 `arch/arm64/mm/mmu.c` 真实硬件大页/CONT 页表）。
- 编写并跑通主机单元测试 `run_vmo_test.py` 与 QEMU EL0 共享 VMO 零拷贝映射实测。

---

### Task 6: `Agent Cell` 五维硬预算、Telemetry 只读页与 64B 强制审计（`SYS_NOS_CELL_CONTROL`）

**Files:**
- Create: `include/nos/cell.h`, `kernel/cap/cell.c`
- Modify: `include/kernel/task.h`, `kernel/task/task.c`, `kernel/sched/sched.c`, `arch/arm64/kernel/trap.c`, `kernel/syscall/syscall.c`
- Modify: `kernel/CMakeLists.txt`, `arch/arm64/kernel/stage2_selftest.c`
- Create: `tests/host/test_agent_cell.c`, `tests/host/run_agent_cell_test.py`

**Targets:**
- 实现 `struct nos_cell`（独立 `CNode` 根、独立 `nos_vm_space`、五维 `nos_cell_budget` 配额原子计数、4KB 只读用户态 `telemetry_page` 映射与 64B `struct nos_audit_record` 内核强制环形审计区）。
- 在 `sch_heartbeat()`（扣除 CPU `cpu_ns_limit`）、VMO 映射（扣除 `mem_bytes_limit`）与 EL0 异常/权限更改门禁中强制原子校验预算并写入不可伪造审计记录；归零时挂起 Cell 并通过 `Notification` 上报 `NOS_AUDIT_BUDGET_HIT`。
- 接入 `SYS_NOS_CELL_CONTROL`（16：`CREATE / DESTROY / SET_BUDGET / GET_BUDGET / FORK / READ_AUDIT`）。
- 编写主机单测 `run_agent_cell_test.py` 并在真实 4 核 `qemu-system-aarch64` 运行完整 Stage 2 端到端闭环实测（Supervisor Cell → 派生受限 Child Agent Cell → 共享 SEAL 权重 VMO → EL0 Telemetry 零 syscall 读预算 → 触发硬预算/Data Abort 强制拦截 + CDT 级联回收全部子句柄）。
