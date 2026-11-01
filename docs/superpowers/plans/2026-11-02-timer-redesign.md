# NOS 内核定时器 (Timer) 子系统重构实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans or superpowers:test-driven-development to implement this plan task-by-task.

**Goal:** 修复 `timer_check_handle()` 栈内存踩踏隐患，重构为支持 Per-CPU 隔离、`next_expires` 零锁快路、双模式存储（小内存有序链表 / 64 槽哈希时间轮）、原生周期定时器与 Tickless 查询的内核定时器子系统。

**Architecture:** 引入 `struct timer_base g_timer_bases[CONFIG_NR_CPUS]`，用 `base->lock` 取代单定时器锁，通过 `base->next_expires` 提供 O(1) 无锁 Tick 快路；在默认配置下使用 64 槽位图时间轮实现 O(1) 插入/删除，在 `CONFIG_SMALL_MEMORY=y` 下自动退化为紧凑有序链表。

---

### Task 1: 编写主机回归测试 (`tests/host/test_timer_subsystem.c`)

**Files:**
- Create: `tests/host/test_timer_subsystem.c`
- Modify: `tests/run_host_regressions.py`
- Modify: `tests/test_kernel_correctness.py`

- [ ] **Step 1: 编写覆盖 6 类核心场景的回归测试**
  1. 回调安全重入与栈指针完整性验证（到期后 `list_empty(&t.list)` 为真，回调中安全重启自身/停止其他定时器/修改周期）。
  2. 多定时器乱序插入、同 Tick 到期顺序与 `next_expires` 精确推进验证。
  3. `TIMER_FLAG_PERIODIC` 周期定时器连续触发与落后追赶防风暴验证。
  4. `timer_mod()`、`timer_pending()`、`timer_remaining_ticks()`、`timer_next_timeout_ticks()` 边界验证。
  5. `scheduler_lock_nest > 0` 期间延迟回调但正确维护到期状态。
  6. `dump_timer_to()` 可观测性输出验证。

---

### Task 2: 重构 `include/kernel/timer.h` 与 `kernel/timer.c`

**Files:**
- Modify: `include/kernel/timer.h`
- Modify: `kernel/timer.c`
- Modify: `kernel/cpu/cpu.c`

- [ ] **Step 1: 更新 `include/kernel/timer.h`**
  - 定义 `TIMER_FLAG_ONE_SHOT`、`TIMER_FLAG_PERIODIC`、`TIMER_FLAG_RUNNING`。
  - 移除 `struct timer` 内冗余的 `spinlock_t lock`，新增 `uint16_t flags` 和 `uint8_t cpu`。
  - 声明新增 API：`timer_init_ex`、`timer_start_periodic`、`timer_mod`、`timer_pending`、`timer_remaining_ticks`、`timer_next_timeout_ticks`、`dump_timer_to`。
- [ ] **Step 2: 实现 `kernel/timer.c` Per-CPU 基座与双模式队列**
  - 实现惰性/幂等初始化 `timer_base_get(cpu_id)`，保证在早期 `task_create` 阶段无需额外初始化顺序依赖即可安全工作。
  - 实现 `CONFIG_SMALL_MEMORY` 有序链表与默认 64 槽哈希时间轮 (`buckets[64]` + `bucket_bitmap`)。
  - 实现 `timer_check_handle()` 的 `next_expires` 快路与无栈悬挂安全回调分发。
- [ ] **Step 3: 优化 `kernel/cpu/cpu.c` 心跳相位**
  - 在 `system_heartbeat_process()` 中先调用 `timer_check_handle()` 再调用 `sch_heartbeat()`，让定时器唤醒的任务在同一 Tick 内参与调度与负载采样。

---

### Task 3: 全量回归、四板构建与 NK60-V2 真机刷机验证

**Files:**
- Verify: `tests/run_host_regressions.py`, `wl-001`, `nk60-v2`, `qemu-stm32`, `zj-v3`

- [ ] **Step 1: 运行主机回归与架构边界检查**
- [ ] **Step 2: 构建全部四个板级预设并运行 QEMU 冒烟测试**
- [ ] **Step 3: 使用 `cmake --build --preset nk60-v2 --target flash` 刷入真机并验证运行状态**
