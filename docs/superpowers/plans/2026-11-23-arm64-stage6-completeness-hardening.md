# NOS ARM64 Stage 6：AI-Native 内核完整性补齐与安全硬化实施计划

> **Architecture Spec:** `docs/superpowers/specs/2026-11-22-arm64-ai-native-kernel-design.md`

---

## Stage 6 核心目标与任务拆解

### Task 1: ARM64 硬件底座修复与安全硬化（MPIDR / DW-8250 / AFDBM / PAN + uaccess / PAC+BTI）

- **Files:**
  - Modify: `arch/arm64/boot/start.S`, `arch/arm64/kernel/vectors.S`, `arch/arm64/kernel/switch.S`, `arch/arm64/kernel/sve.S`, `arch/arm64/kernel/el0_smoke.S`
  - Modify: `drivers/serial/uart_dw8250.c`, `tests/host/test_dw8250_uart.c`
  - Modify: `arch/arm64/include/asm/sysreg.h`, `arch/arm64/include/asm/security.h`, `arch/arm64/cpu/security.c`, `arch/arm64/include/asm/percpu.h`
  - Create: `include/nos/uaccess.h`, `arch/arm64/kernel/uaccess.S`, `arch/arm64/kernel/uaccess.c`
  - Modify: `arch/arm64/CMakeLists.txt`, `board/qemu-virt/nos_kernel.ld.S`, `board/rk3588/nos_kernel.ld.S`, `arch/arm64/kernel/trap.c`, `kernel/syscall/syscall.c`
  - Modify: `arch/arm64/compiler_flags.cmake`, `arch/arm64/mm/mmu.c`, `kernel/loader/elf64_loader.c`, `user/CMakeLists.txt`, `user/apps/ai_agent_demo/crt0.S`, `app/shell/ai_cmds.c`
- **Targets:**
  - [x] **1.1:** 修复 `start.S` 多核 MPIDR 24 位掩码（`0xFFFFFF`）与 BTI 跳转 register (`x16`)，汇编可间接调用符号增加 `bti c`（`hint #34`）。
  - [x] **1.2:** 修复 `uart_dw8250.c` Busy Detect (`IIR=0x07`) 读取 `USR` 清中断并在 `test_dw8250_uart.c` 验证。
  - [x] **1.3:** 使能 `AFDBM` (`TCR_EL1_HA|HD`) 与 `PAN` (`SCTLR_EL1_SPAN=0` + `msr pan, #1`)，实现 `__ex_table` + `nos_copy_from_user`/`nos_copy_to_user`/`nos_strncpy_from_user` 并全面改造 `kernel/syscall/syscall.c`。
  - [x] **1.4:** 启用 EL0 完整 `-mbranch-protection=standard`（PAC+BTI+`HAL_VM_PROT_BTI`）与 EL1 `-mbranch-protection=bti`（内核代码段开 `GP`）。

---

### Task 2: 补齐 `Agent Cell` 与 `CDT` 完整语义

- **Files:**
  - Modify: `include/nos/cap.h`, `kernel/cap/cap_table.c`
  - Modify: `include/uapi/nos/ai.h`, `include/kernel/task.h`, `kernel/task/task.c`, `kernel/sched/sched.c`
  - Modify: `include/nos/cell.h`, `kernel/ai/cell.c`, `kernel/syscall/syscall.c`
  - Modify/Create: `arch/arm64/include/asm/mmu.h`, `arch/arm64/mm/mmu.c`
- **Targets:**
  - [x] **2.1:** 实现 CNode `schema_mask`、表快照与恢复（`nos_cap_table_snapshot/restore`）、Fork 能力树派生（`nos_cap_table_clone_for_fork`）及撤销在途排空回调机制。
  - [x] **2.2:** 实现 `NOS_CELL_OP_FORK` 真 Fork（共享只读权重 + 可写页 CoW + 父级剩余预算夹紧 + 派生能力）、`Snapshot` 冻结 CNode 并在 `Rollback` 时恢复（含批量 ASID TLBI 加速）、`NOS_CELL_OP_BIND_NOTIF` 暴露、`NOS_CELL_EVT_BUDGET_*` 事件区分及 CPU 超限挂起 Cell 全部线程（`task_suspend_cell_tasks`）。

---

### Task 3: 补齐 `VMO / TensorObj` 大页、权重指纹、按需缺页、`AF` 位驱逐与 `SYNC_DMA`

- **Files:**
  - Modify: `include/uapi/nos/ai.h`, `include/nos/vmo.h`, `kernel/mm/vmo.c`
  - Modify: `arch/arm64/include/asm/mmu.h`, `arch/arm64/mm/mmu.c`, `kernel/ai/cell.c`, `arch/arm64/kernel/trap.c`, `kernel/syscall/syscall.c`
- **Targets:**
  - [x] **3.1:** 实现高阶连续物理块分配器（`nos_vmo_phys_block`，64KB order=4 / 2MB order=9 按块分配+独立 4KB 页 CoW 分裂及降级回退）与 `NOS_VMO_FLAG_LAZY_ALLOC` 按需分配。
  - [x] **3.2:** 实现 `nos_vmo_seal()` FNV-1a 权重哈希指纹、`nos_vmo_sync_dma` + `NOS_VMO_OP_SYNC_DMA`、`arm64_pt_test_and_clear_af()`、`nos_vmo_reclaim_cold_pages()` 及 `nos_cell_handle_page_fault()` 按需/可重计算页恢复。

---

### Task 4: 补齐 `RingIPC` EL0 服务端闭环、`CANCEL` 与 `MOVE_SLICE`

- **Files:**
  - Modify: `include/uapi/nos/ai.h`, `include/nos/ring.h`, `kernel/ipc/ring_ipc.c`, `kernel/syscall/syscall.c`
- **Targets:**
  - [x] **4.1:** 联动 CNode `schema_mask` 校验；实现 `NOS_RING_OP_CANCEL` 队列扫描与加速器转发取消；实现 `NOS_RING_FLAG_MOVE_SLICE` 句柄转移派生+源撤销。
  - [x] **4.2:** 实现 `NOS_RING_ENTER_BIND_SERVER`、`NOS_RING_ENTER_SERVER_POP` 与 `NOS_RING_ENTER_COMPLETE`，打通 EL0↔EL0 跨 Cell 异步 Tool Server，并注册 CDT 撤销排空回调。

---

### Task 5: 补齐 `sched_infer` 真 big.LITTLE EAS、Gang 自动协同换入与 `oneshot_ns` 定时抢占

- **Files:**
  - Modify: `arch/arm64/include/asm/fdt_scan.h`, `arch/arm64/kernel/fdt_scan.c`, `arch/arm64/include/asm/cpufeature.h`, `arch/arm64/cpu/cpufeature.c`
  - Modify: `include/uapi/nos/ai.h`, `include/kernel/task.h`, `kernel/sched/sched_infer.c`, `kernel/syscall/syscall.c`, `arch/arm64/kernel/timer.c`
- **Targets:**
  - [x] **5.1:** 解析运行时 FDT 与静态 DTS 的 `capacity-dmips-mhz`，实现 `arm64_topo_classify_masks()` 与 `DECODE` 访存核簇打散策略，使 EAS 与 `arch_sched_query_topo()` 真生效。
  - [x] **5.2:** 暴露 `NOS_SCHED_OP_GANG_WAKEUP` 并在唤醒/入队 paths 协同通知同组 Gang 核；接入 `hal_timer_program_oneshot_ns()` 实现无时钟漂移影响的微秒级 CBS 精准抢占。

---

### Task 6: 补齐 `SMMUv3` EVTQ 故障中断/TLBI 联动与 `AccelQueue` 完整操作集

- **Files:**
  - Modify: `include/nos/iommu.h`, `arch/arm64/include/asm/smmuv3.h`, `arch/arm64/mm/smmuv3.c`, `arch/arm64/kernel/fdt_scan.c`, `arch/arm64/hal/hal_arm64.c`, `kernel/ai/cell.c`
  - Modify: `include/uapi/nos/ai.h`, `include/nos/accel.h`, `kernel/accel/accel_core.c`, `drivers/accel/virtio_accel.c`, `drivers/accel/rknpu.c`, `kernel/syscall/syscall.c`
- **Targets:**
  - [x] **6.1:** SMMUv3 开启 EVTQ/GERROR 中断与 `nos_iommu_poll_events()`，联动 `Cell` 写入 `NOS_AUDIT_IOMMU_FAULT` 与事件通知；`hal_vm_unmap`/`hal_vm_space_destroy` 自动广播 IOMMU TLBI。
  - [x] **6.2:** 实现 `NOS_ACCEL_FLAG_ASYNC` 与 `NOS_ACCEL_OP_WAIT` / `CANCEL` / `RESET_CTX`（AccelCore + VirtIO-Accel 池化队列 + RKNPU 多页 IOMMU `.bind_vmo`/寄存器序列编码）。

---

### Task 7: 用户态升级（PIE + `argv` + 迷你 `ulibc` + EL0 Tool Server）与全矩阵验证

- **Files:**
  - Modify: `include/nos/elf64.h`, `kernel/loader/elf64_loader.c`, `app/shell/ai_cmds.c`, `tests/host/test_elf64_loader.c`
  - Modify: `user/include/nos/user_ai.h`, `user/CMakeLists.txt`, `user/apps/ai_agent_demo/main.c`, `user/libnos/builtin_apps_fs.c`
  - Create: `user/include/nos/ulibc.h`, `user/apps/tool_server_demo/{crt0.S,user.ld,main.c}`
  - Create: `tests/host/test_stage6_completeness.c`, `tests/host/run_stage6_completeness_test.py`
  - Modify: `arch/arm64/kernel/stage2_selftest.c`, `tests/qemu_arm64_smoke.py`, `tests/run_host_regressions.py`, `tests/host/test_arm64_mmu.c`, `tests/host/test_arm64_smmuv3.c`
- **Targets:**
  - [x] **7.1:** ELF64 加载器支持 `ET_DYN` (PIE `R_AARCH64_RELATIVE` 重定位) 与 `argv` 传递。
  - [x] **7.2:** 补全 `user_ai.h` 全量 syscall 封装，创建 `ulibc.h`（VMO 堆 + 字符串/打印），升级 `ai_agent_demo` 与新增 `/bin/tool_srv.elf` 演示 EL0 RingIPC 服务。
  - [x] **7.3:** 完成 `test_stage6_completeness.c`、`stage2_selftest.c` (`run_stage6_selftest`) 与全部 6 Preset 零警告构建、全主机回归及 6 组 QEMU 冒烟验证。
