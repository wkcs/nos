# NOS ARM64 Stage 3：`RingIPC`、硬件 CoW `Snapshot/Rollback`、`sched_infer` 与惰性 NEON 上下文实施计划

> **Architecture Spec:** `docs/superpowers/specs/2026-11-22-arm64-ai-native-kernel-design.md`

---

## Stage 3 核心目标与任务拆解

### Task 1: 真惰性 ARM64 NEON/FP 上下文切换 + `sched_infer`（EDF / CBS / 截止期继承 / EAS / Gang）

- **Files:**
  - Modify: `arch/arm64/include/asm/task.h`, `include/kernel/task.h`, `arch/arm64/kernel/switch.S`, `arch/arm64/cpu/fpu.c`
  - Modify: `include/kernel/sched.h`, `include/nos/sched.h`, `kernel/sched/sched_rt.c`, `kernel/sched/sched.c`, `kernel/task/task.c`, `arch/arm64/cpu/cpufeature.c`, `kernel/CMakeLists.txt`
  - Create: `include/nos/sched_infer.h`, `kernel/sched/sched_infer.c`
  - Create: `tests/host/test_sched_infer.c`, `tests/host/run_sched_infer_test.py`
- **Targets:**
  - [x] **Step 1:** 实现 `struct arm64_fp_state`（64 个 `uint64_t` 对应 `q0..q31` + `fpsr`/`fpcr`/`valid`）与汇编快速保存/恢复/清零原语 `arm64_fp_save`/`arm64_fp_restore`/`arm64_fp_zero`；切出核时若 `fpu_owner` 脏则在退出前保存（无跨核 IPI 竞态），陷阱命中后恢复或清零。
  - [x] **Step 2:** 新增 `SCHED_POLICY_INFER = 4` 与 `infer_sched_class`（优先级介于 `rt_sched_class` 与 `fair_sched_class` 之间），实现按 `abs_deadline_ns` 升序排列的 EDF 调度队列与 CBS 带宽节流自动周期补给。
  - [x] **Step 3:** 实现 `sched_infer_boost_deadline` / `sched_infer_restore_deadline` 截止期继承、16 组 `nos_gang_group` 多核同步唤醒与基于 `arch_sched_query_topo()` 的角色异构核（EAS）亲和选核。
  - [x] **Step 4:** 通过主机 TDD 测试 `run_sched_infer_test.py` 且现有 `run_sched_multi_policy_test.py` / `run_timer_test.py` 零回归。

---

### Task 2: 硬件 CoW 亚毫秒 `Agent Cell` 检查点快照与回滚（`NOS_CAP_SNAPSHOT`）

- **Files:**
  - Modify: `include/uapi/nos/ai.h`, `kernel/cap/cap_table.c`, `include/nos/vmo.h`, `kernel/mm/vmo.c`
  - Modify: `include/nos/cell.h`, `kernel/ai/cell.c`, `arch/arm64/kernel/trap.c`
  - Modify: `tests/host/test_vmo.c`, `tests/host/test_agent_cell.c`
- **Targets:**
  - [x] **Step 1:** 在 `nos_vmo` 新增按页 CoW 断裂原语 `nos_vmo_cow_break_page()` 与零拷贝恢复原语 `nos_vmo_restore_from_snapshot()`；Cell 跟踪最多 16 个已映射 VMO 绑定范围。
  - [x] **Step 2:** 实现 `nos_cell_snapshot()`（零拷贝克隆并封印全部可写 VMO 绑定，将对应现网 PTE 降级为只读）与 `nos_cell_handle_cow_fault()`（EL0 写入 CoW 页触发 Data Abort 时透明拆分私有页并恢复 `RW-`，仅真正越界/只读页保持 Fault）。
  - [x] **Step 3:** 实现 `nos_cell_rollback()`（恢复所有绑定 VMO 物理页与页表、还原预算已用量与状态并写审计项 `NOS_AUDIT_CELL_ROLLBACK`）。
  - [x] **Step 4:** 通过 `test_agent_cell.c` 与 `test_vmo.c` 单测覆盖验证。

---

### Task 3: `RingIPC` 异步零拷贝 SQ/CQ 共享环、Schema 白名单与系统调用接线（`16/19/20/21`）

- **Files:**
  - Create: `include/nos/ring.h`, `kernel/ipc/ring_ipc.c`
  - Modify: `kernel/CMakeLists.txt`, `kernel/syscall/syscall.c`, `arch/arm64/kernel/trap.c`
  - Create: `tests/host/test_ring_ipc.c`, `tests/host/run_ring_ipc_test.py`
  - Modify: `tests/run_host_regressions.py`
- **Targets:**
  - [x] **Step 1:** 扩展 [ai.h](file:///Volumes/nick-disk/projects/nos/include/uapi/nos/ai.h) 新增 128B 双缓存行 `struct nos_ring_header`、32B `struct nos_ring_create_args` 与 32B `struct nos_cpu_topo_info` 及对应 `_Static_assert`。
  - [x] **Step 2:** 实现 `RingIPC` 共享 VMO 环形队列（Acquire/Release 内存序无锁生产消费）、16 槽位 Schema 白名单门禁、绑定 Cell `tool_calls`/`accel_ops` 硬配额扣费、零拷贝 VMO 切片校验、服务端 `Deadline Inheritance` 继承与 `Notification` 唤醒。
  - [x] **Step 3:** 接入 `SYS_NOS_CELL_CONTROL` (`SNAPSHOT/ROLLBACK`)、`SYS_NOS_RING_CREATE` (19)、`SYS_NOS_RING_ENTER` (20)、`SYS_NOS_SCHED_INFER` (21) 及全部 EL0 页表指针/区间校验。
  - [x] **Step 4:** 通过主机单测 `run_ring_ipc_test.py`。

---

### Task 4: QEMU 4 核 ARM64 端到端硬件实测与全矩阵回归

- **Files:**
  - Modify: `arch/arm64/kernel/el0_smoke.S`, `arch/arm64/kernel/stage2_selftest.c`, `tests/qemu_arm64_smoke.py`
- **Targets:**
  - [x] **Step 1:** 新增 `arm64_el0_neon_worker` 与 `arm64_el0_stage3_code` PIC 测试载荷与 `run_stage3_selftest()`：实测硬件 CoW 快照/写拆分/亚毫秒回滚耗时、双并发 EL0 NEON 线程上下文隔离、`RingIPC` 批量工具调用+白名单拦截+截止期继承及异构拓扑查询。
  - [x] **Step 2:** 跑通 5 个预设（ARM64 + 4 个 Cortex-M MCU）零警告编译、4 种 QEMU ARM64 配置、STM32 QEMU、层边界检查与全部主机/Python 回归套件。
