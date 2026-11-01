# NOS 内核定时器 (Timer) 子系统重构设计规范

- **日期**: 2026-11-02
- **状态**: 已批准
- **涉及模块**: `include/kernel/timer.h`, `kernel/timer.c`, `kernel/cpu/cpu.c`, `tests/host/test_timer_subsystem.c`

## 1. 背景与问题诊断

对当前 `kernel/timer.c` 和 `include/kernel/timer.h` 的审查发现以下关键问题：

1. **栈内存踩踏缺陷 (Silent Stack Corruption)**：
   - `timer_check_handle()` 将到期定时器挂入栈上局部链表 `LIST_HEAD(tmp_list)`，但在遍历执行 `timer->timeout_func()` 时未调用 `list_del_init(&timer->list)`。
   - 函数返回后，`timer->list` 的 `prev/next` 仍指向已销毁的栈地址 `&tmp_list`。后续任何对该定时器的 `timer_start()` / `timer_remove()` 调用都会向已释放的栈内存写入指针，若回调内重装自身定时器更会直接破坏正在遍历的 `tmp_list`。
2. **每 Tick 无条件关中断加锁**：
   - `timer_check_handle()` 在每个 SysTick 中断中无条件执行两次 `spin_lock_irq(&g_timer_list_lock)`，缺少 `next_expires` 原子快路。
3. **插入期间时间基准漂移与 O(N) 遍历开销**：
   - `timer_start()` 在遍历链表时反复调用 `get_lave_tick()` 读取动态变化的 `cpu_run_ticks()`，跨 Tick 时会破坏链表有序性。
   - 全局单链表在多连接/多任务下存在 O(N) 插入与单锁竞争。
4. **能力缺失与心跳相位倒置**：
   - 缺少原生周期定时器 (`TIMER_FLAG_PERIODIC`)、`timer_pending()`、`timer_mod()`、`timer_remaining_ticks()` 以及供 Tickless 低功耗使用的 `timer_next_timeout_ticks()`。
   - `system_heartbeat_process()` 先调用 `sch_heartbeat()` 再调用 `timer_check_handle()`，导致同 Tick 到期唤醒的任务滞后一个调度检查相位。

## 2. 架构设计

### 2.1 Per-CPU 定时器基座 (`struct timer_base`)
- 每个 CPU 核心维护独立的 `struct timer_base`（单核 MCU 下 `CONFIG_NR_CPUS == 1`，仅单实例）。
- `struct timer_base` 包含：
  - `spinlock_t lock`：保护本核定时器队列，取代每个 `struct timer` 内部冗余的 `spinlock_t`。
  - `uint64_t next_expires`：本核最近到期定时器的绝对 Tick（无定时器时为 `U64_MAX`）。
  - `uint32_t active_count` / `uint64_t expired_count`：活跃定时器计数与累计触发统计。
  - **双模式存储容器**：
    - `CONFIG_SMALL_MEMORY=y`（如 `wl-001`）：单条按 `timeout_tick` 升序排列的 `struct list_head active_list`，极致节省 RAM。
    - 默认模式（如 `nk60-v2`、SMP 多核平台）：`TIMER_WHEEL_BUCKETS`（64 槽，2 的幂掩码）哈希时间轮 `struct list_head buckets[64]` + 64 位非空槽位位图 `uint64_t bucket_bitmap`，实现 O(1) 入队与删除。

### 2.2 定时器对象 (`struct timer`)
- 字段定义：
  - `const char *name`：定时器名称。
  - `uint32_t init_tick`：初始或周期 Tick 数。
  - `uint64_t timeout_tick`：64 位绝对到期 Tick（消除 32 位回绕歧义）。
  - `void (*timeout_func)(void *parameter)`：到期回调。
  - `void *parameter`：回调参数。
  - `struct list_head list`：侵入式链表节点（始终保持 `list_empty(&timer->list)` 与未激活状态一致）。
  - `uint16_t flags`：`TIMER_FLAG_ONE_SHOT (0x00)`、`TIMER_FLAG_PERIODIC (0x01)`、`TIMER_FLAG_RUNNING (0x02)`。
  - `uint8_t cpu`：所属 CPU 核心号。
  - `bool timeout`：兼容旧代码的到期标志。

### 2.3 `next_expires` 零锁快路与安全回调状态机
1. **快路检查**：
   - `timer_check_handle()` 先读取当前 `now_tick = cpu_run_ticks()`，若 `now_tick < READ_ONCE(base->next_expires)` 则直接返回，99% 空闲 Tick 零加锁、零关中断。
2. **安全到期摘除与周期重装**：
   - 当 `now_tick >= base->next_expires` 且 `scheduler_lock_nest == 0` 时，获取 `base->lock`。
   - 收集到期定时器（`timeout_tick <= now_tick`）：
     - 先执行 `list_del_init(&timer->list)` 彻底从桶/链表解链。
     - 若设置 `TIMER_FLAG_PERIODIC`，计算下一个周期 `timer->timeout_tick += timer->init_tick`（若仍 `<= now_tick` 则对齐到 `now_tick + timer->init_tick`），立即重新插入 `base`。
     - 在调用 `timeout_func` 前已确保非周期定时器 `list_empty(&timer->list)` 为真，彻底杜绝栈悬挂指针。
   - 更新 `base->next_expires`。
   - 释放 `base->lock` 后依次调用回调，回调内可安全调用 `timer_start()`、`timer_stop()`、`timer_mod()` 或 `kfree()`。

### 2.4 API 扩展与心跳时序优化
- 保持原有 API 100% 兼容：`timer_init`、`timer_creat`、`timer_remove`、`timer_start`、`timer_stop`、`timer_check_handle`、`dump_timer`。
- 新增增强 API：
  - `int timer_init_ex(struct timer *timer, const char *name, void (*timeout)(void *), void *parameter, uint16_t flags)`
  - `int timer_start_periodic(struct timer *timer, u32 period_tick)`
  - `int timer_mod(struct timer *timer, u32 tick)`
  - `bool timer_pending(const struct timer *timer)`
  - `u32 timer_remaining_ticks(const struct timer *timer)`
  - `u64 timer_next_timeout_ticks(void)`
  - `void dump_timer_to(int (*print_fn)(const char *fmt, ...))`
- 在 `kernel/cpu/cpu.c:system_heartbeat_process()` 中，先执行 `timer_check_handle()` 再执行 `sch_heartbeat()`，让定时器唤醒的任务在同一 Tick 内被调度器感知并统一完成抢占切换。
