# NOS IPC Wait and Lifecycle Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现已批准的第一阶段 IPC 等待与生命周期底座，并将 mutex 接入；以生产代码的受控交错及主机多线程测试验证 MCU/SMP 共用协议。

**Architecture:** 任务锁保护等待身份，对象锁裁决唯一终态，锁外发布带代数的调度唤醒。Tagged timer 将不可变事件和任务引用交给 dispatcher；每 CPU 独立记录抢占/IRQ 状态，reaper 只回收没有在途引用的对象和任务。旧 IPC 先适配引用、代数和唤醒节点，其资源语义分别在第二、三阶段迁移。

**Tech Stack:** GNU C11、既有 NOS CMake/Ninja/Kconfig 构建、Python unittest/host runner、pthread 主机执行环境、ARM GCC、QEMU STM32；不增加固件运行时第三方库。

**Approved spec:** [第一阶段设计规范](../specs/2026-11-02-ipc-wait-foundation-design.md)，2026-11-02 用户回复“没问题”批准。

---

## 执行边界与顺序

- 本计划只执行第一阶段。第二阶段的原语 API/调用方迁移、第三阶段的 Endpoint/Capability 事务分别另写规范和计划。
- 在独立分支/工作树执行，不能直接在 master 上实施。建议位置为仓库已经忽略的 `.worktrees/ipc-wait-foundation`，分支 `feature/ipc-wait-foundation`；目录和执行方式在交接时一次确认。
- 按 Task 1–14 顺序执行。Task 2–4 是底层契约，Task 5–10 是等待闭环，Task 11–13 接入真实 mutex，Task 14 做整体验收。
- 每个任务遵循 red → green → focused regression → commit；只在引入行为的测试已经失败后写对应实现。失败应是新行为未实现，而不是工具未安装或测试自身语法错误。
- 代码使用 4 空格、K&R、定宽类型、版权/作者块；不要顺手重排已有 CRLF 的 `include/kernel/list.h`。
- 任何跨阶段设计变化先记录证据和规范修订。不能删掉并发断言、换成无锁 stub、以增加 sleep 来掩盖竞态。

## 文件分工

| 文件 | 职责 |
| --- | --- |
| `include/nos/cpu_context.h`, `kernel/cpu/context.c` | CPU 本地抢占/IRQ 访问、阻塞上下文检查 |
| `include/nos/timeout.h`, `kernel/ipc/timeout.c` | timeout → 绝对 deadline；使用 `cpu_run_ticks()` 的同步快照 |
| `include/nos/refcount.h` | 有界 32 位引用操作，不依赖 64 位原子 |
| `include/nos/object.h`, `kernel/ipc/object.c` | 对象锁、close/retire 和任务上下文退休队列 |
| `include/nos/wait.h`, `kernel/ipc/wait.c` | 等待记录、排序、唯一结果、wake batch、park/finish |
| `include/nos/timer_event.h`, `kernel/timer.c` | tagged 事件及引用所有权转移；保留普通 timer API |
| `include/nos/task_wait.h`, `kernel/task/task_wait.c` | task 引用、等待初始化、managed/legacy timeout、兼容 wake 队列 |
| `kernel/task/task.c` | 创建、取消、退出和 reaper 接入；保留现有任务调度/诊断主体 |
| `kernel/sched/sched.c`, `include/nos/sched.h` | 准备睡眠、带 epoch 唤醒、源 rq 重验证 |
| `kernel/ipc/mutex.c`, `kernel/ipc/mutex_pi.c`, `include/nos/mutex_pi.h` | mutex ownership/交接与独立的有界 PI 传播 |
| `tests/host/ipc_support/*` | 原子锁、TLS CPU、可控 HAL 切换和同步测试闸门；不实现被测原语 |
| `tests/host/test_ipc_*.c`, `tests/host/run_ipc_test.py` | 分组件测试和生产代码集成测试 |
| `tests/host/report_ipc_cost.py`, `docs/superpowers/reviews/2026-11-02-ipc-wait-foundation-verification.md` | 固件/TCB 成本、测试证据与明确的遗留项 |

新增生产 `.c` 同任务加入 `kernel/CMakeLists.txt`，新增 host suite 同任务加入 `tests/run_host_regressions.py`。每个提交至少能通过自己的 focused runner；任务边界不保留无法链接的生产对象。

## 公共内部契约清单

下列名称在对应任务中定义；测试与后续任务使用同一签名。所有 `_locked` 接口要求调用方满足规范锁序；普通入口自行取得必要的锁。

```c
/* Task 2: include/nos/cpu_context.h */
uint32_t nos_preempt_depth(void);
uint32_t nos_irq_depth(void);
void nos_preempt_disable(void);
void nos_preempt_enable(void);
void nos_irq_enter(void);
void nos_irq_exit(void);
bool nos_can_block(void);

/* Task 3: include/nos/timeout.h */
typedef uint32_t nos_timeout_t;
#define NOS_NO_WAIT UINT32_C(0)
#define NOS_WAIT_FOREVER UINT32_MAX
struct nos_deadline {
    uint64_t tick;
    bool finite;
    bool no_wait;
};
int nos_deadline_from_ticks(nos_timeout_t timeout, uint64_t now,
                            struct nos_deadline *deadline);
bool nos_deadline_expired(const struct nos_deadline *deadline, uint64_t now);

/* Task 4: include/nos/refcount.h and include/nos/object.h */
struct nos_refcount { uint32_t value; };
void nos_ref_init(struct nos_refcount *ref);
bool nos_ref_get(struct nos_refcount *ref);
bool nos_ref_put(struct nos_refcount *ref);
uint32_t nos_ref_read(const struct nos_refcount *ref);
enum nos_object_storage { NOS_OBJECT_STATIC, NOS_OBJECT_DYNAMIC };
struct nos_object {
    spinlock_t lock;
    struct nos_refcount refs;
    struct list_head retire_node;
    void (*release)(struct nos_object *object);
    enum nos_object_storage storage;
    bool closed;
    bool owner_released;
};
void nos_object_init(struct nos_object *object, enum nos_object_storage storage,
                     void (*release)(struct nos_object *));
bool nos_object_get(struct nos_object *object);
void nos_object_put(struct nos_object *object);
int nos_object_retire(struct nos_object *object);
void nos_object_reap(void);

/* Task 5: include/nos/timer_event.h */
struct nos_timer_ticket {
    void (*callback)(void *argument, uint64_t tag);
    void (*release)(void *argument);
    void *argument;
    uint64_t tag;
};
int timer_init_tagged(struct timer *timer, const char *name, uint8_t cpu);
int timer_start_tagged(struct timer *timer, uint64_t deadline,
                       const struct nos_timer_ticket *owned_ticket);
int timer_cancel_tag(struct timer *timer, uint64_t tag,
                     struct nos_timer_ticket *detached_ticket);
void timer_ticket_release(struct nos_timer_ticket *ticket);

/* Task 6: include/nos/task_wait.h */
bool task_get(struct task_struct *task);
void task_put(struct task_struct *task);
int nos_task_wait_init(struct task_struct *task, uint8_t timer_cpu);
int nos_task_next_epoch_locked(struct task_struct *task, uint64_t *epoch);
bool nos_task_reapable_locked(struct task_struct *task);

/* Task 7: include/nos/wait.h */
enum nos_wait_state {
    NOS_WAIT_IDLE, NOS_WAIT_PENDING, NOS_WAIT_SUCCEEDED,
    NOS_WAIT_TIMEDOUT, NOS_WAIT_CLOSED, NOS_WAIT_CANCELLED
};
struct nos_wait_queue {
    struct nos_object *object;
    struct list_head waiters;
    uint64_t next_order;
    void (*changed_locked)(struct nos_wait_queue *queue);
    void (*settle)(struct nos_object *object);
};
struct nos_wake_batch { struct list_head tasks; };
struct nos_wait_context {
    struct list_head wait_node;
    struct list_head wake_node;
    struct task_struct *task;
    struct nos_object *object;
    struct nos_wait_queue *queue;
    struct nos_deadline deadline;
    uint64_t epoch;
    uint64_t order;
    enum nos_wait_state state;
    int result;
    uint8_t rank_class;
    uint8_t rank_priority;
    void *data;
};
void nos_wait_queue_init(struct nos_wait_queue *queue, struct nos_object *object);
void nos_wake_batch_init(struct nos_wake_batch *batch);
int nos_wait_prepare_locked(struct task_struct *task, struct nos_wait_queue *queue,
                            const struct nos_deadline *deadline, void *data);
bool nos_wait_complete_locked(struct nos_wait_context *wait,
                              enum nos_wait_state state, int result,
                              uint64_t now, struct nos_wake_batch *batch);
void nos_wait_reorder_locked(struct task_struct *task);
void nos_wait_park(struct task_struct *task, uint64_t epoch);
int nos_wait_finish(struct task_struct *task, uint64_t epoch);
void nos_wake_flush(struct nos_wake_batch *batch);

/* Task 8: include/nos/sched.h */
int sched_wait_prepare_locked(struct task_struct *task);
int sched_wake_epoch(struct task_struct *task, uint64_t epoch);

/* Task 9: include/nos/task_wait.h */
int nos_task_deadline_arm_locked(struct task_struct *task);
void nos_task_deadline_cancel(struct task_struct *task, uint64_t epoch);
void nos_wait_timeout(void *argument, uint64_t epoch);

/* Task 10: include/nos/task_wait.h */
int task_legacy_timeout_start_locked(struct task_struct *task, uint32_t ticks);
void task_legacy_timeout_cancel(struct task_struct *task, uint64_t epoch);
void task_legacy_wake_enqueue_locked(struct task_struct *task);
void task_legacy_wake_flush(void);

/* Task 11: include/kernel/mutex.h */
int mutex_acquire(struct mutex *lock, nos_timeout_t timeout);

/* Task 12: include/nos/mutex_pi.h */
void mutex_pi_changed_locked(struct mutex *lock);
void mutex_pi_recompute(struct task_struct *task);
void mutex_pi_propagate(struct task_struct *task);

/* Task 13: include/kernel/task.h */
int task_cancel(struct task_struct *task);
/* Task 13: include/nos/wait.h */
int nos_wait_abandon(struct task_struct *task, uint64_t epoch);
```

`task` 增加 refs、wait、wait_timer、wait_epoch、wake_epoch、legacy_epoch、legacy_wake_epoch、legacy_wake_node、reap_node、exiting、legacy_wake_queued 和 PI 来源/序号字段。`wait.object != NULL` 表示 managed 等待仍在活动；即使 state 已终态也不得提前重用。

## Task 1: 建立真实生产代码的主机测试入口

**Files:** Create `tests/host/run_ipc_test.py`, `tests/host/ipc_support/host.h`, `tests/host/ipc_support/host.c`, `tests/host/test_ipc_harness.c`.

- [ ] **Step 1: 建立 runner 和原子锁 HAL。** 复用 `run_sched_multi_policy_test.py` 的 `write_host_asm_stubs()` 生成基础头，但覆盖其非原子 spinlock；不能将该旧 stub 用于并发 suite。新 host arch-spinlock 使用以下完整的原子锁操作，维持 NOS 所需的 union 布局：

```c
#include <stdbool.h>
#include <stdint.h>
#include <sched.h>

#define ARCH_HAS_SPIN_TRYLOCK 1
typedef union {
    uint32_t slock;
    struct { uint16_t owner, next; } tickets;
} arch_spinlock_t;
static inline int arch_spin_is_locked(const arch_spinlock_t *lock)
{
    return __atomic_load_n(&lock->slock, __ATOMIC_RELAXED) != 0U;
}
static inline void arch_spin_lock(arch_spinlock_t *lock)
{
    while (__atomic_exchange_n(&lock->slock, 1U, __ATOMIC_ACQUIRE) != 0U) {
        sched_yield();
    }
}
static inline void arch_spin_unlock(arch_spinlock_t *lock)
{
    __atomic_store_n(&lock->slock, 0U, __ATOMIC_RELEASE);
}
static inline int arch_spin_trylock(arch_spinlock_t *lock)
{
    uint32_t expected = 0U;
    return __atomic_compare_exchange_n(&lock->slock, &expected, 1U, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
```

- [ ] **Step 2: 实现 host 工具契约。** `host.h` 声明 `ipc_host_bind_cpu(uint32_t)`、`ipc_host_set_ticks(uint64_t)`、`ipc_host_set_current(struct task_struct *)`、`ipc_host_irq_state(void)`；host.c 用 TLS CPU/IRQ、同步逻辑时钟和按 CPU 记录的 IPI/切换计数提供 HAL。对当前 suite 没有链接的无关 MM/日志/设备符号提供显式 stub，禁止给 wait/mutex/timer/引用/调度状态转移提供成功空桩。

```c
/* suite-local test; existing NOS spinlock calls must be real. */
static void test_host_irq_roundtrip(void)
{
    spinlock_t lock;
    spin_lock_init(&lock);
    assert(ipc_host_irq_state() == 0U);
    spin_lock_irq(&lock);
    assert(ipc_host_irq_state() != 0U);
    spin_unlock_irq(&lock);
    assert(ipc_host_irq_state() == 0U);
}
```

- [ ] **Step 3: runner 接受 `suite --cpus N --small-memory --sanitize MODE`。** suite 为 `all/harness/cpu/timeout/object/timer/task/wait/sched/integration/legacy/mutex/pi/cancel/stress`；按 suite 显式选择生产源文件。编译使用 `-std=gnu11 -O1 -g -Wall -Wextra -pthread` 和真实 `include/`，只有 `asm/`、平台配置及无关板级入口可替换。进程超时 30 秒，stress 为 120 秒；失败保留 stdout/stderr。
- [ ] **Step 4: 运行 `python3 tests/host/run_ipc_test.py harness --cpus 1`。** 先使原子锁压力断言在错误的无锁 HAL 下失败，再启用上述原子实现，确认计数器准确、IRQ 嵌套恢复和测试超时有效。
- [ ] **Step 5: 提交。** `git add tests/host/run_ipc_test.py tests/host/ipc_support tests/host/test_ipc_harness.c`，`git commit -m 'Tests: Add production IPC host harness'`。

## Task 2: CPU 本地抢占、IRQ 上下文及 timer-base 初始化

**Files:** Create `include/nos/cpu_context.h`, `kernel/cpu/context.c`, `tests/host/test_ipc_cpu.c`; modify `include/kernel/sched.h`, `kernel/sched/sched.c`, `kernel/irq/irq.c`, `include/kernel/irq.h`, `include/kernel/spinlock.h`, `kernel/timer.c`, `kernel/CMakeLists.txt`, `tests/host/test_timer_subsystem.c`, `tests/host/test_sched_load_dump.c`, their runners.

- [ ] **Step 1: 增加失败用例。** CPU0 在闸门前调用 sch_lock，CPU1 不得观察到非零深度；两线程各自增减 100000 次，深度最终为零。单线程执行以下行为断言：

```c
ipc_host_bind_cpu(0U);
assert(nos_preempt_depth() == 0U);
sch_lock();
assert(nos_preempt_depth() == 1U);
nos_irq_enter();
assert(nos_irq_depth() == 1U);
assert(!nos_can_block());
nos_irq_exit();
sch_unlock();
assert(nos_preempt_depth() == 0U);
```

- [ ] **Step 2: 跑 `python3 tests/host/run_ipc_test.py cpu --cpus 2`，确认缺失新接口或全局深度污染导致 RED。**
- [ ] **Step 3: 实现每 CPU 状态。** 固件静态数组，主机 TLS；外部 CPU ID 需校验。深度读改写保存/恢复本地 IRQ；sch_lock/unlock 改调新函数，零深度不直接 switch。`get_sch_lock_level()` 返回当前 CPU 深度。更新 scheduler/timer 中所有裸 `scheduler_lock_nest` 引用和旧测试，删除原全局定义。

```c
/* include/kernel/sched.h: replacement wrappers */
static inline void sch_lock(void)
{
    nos_preempt_disable();
}
static inline void sch_unlock(void)
{
    nos_preempt_enable();
}
```

- [ ] **Step 4: 接入 IRQ 和 can_block。** IRQ 深度区分硬 IRQ 与 syscall；Cortex-M 读取 IPSR，保留 UP 汇编兼容符号；其他架构只能在接入 trap 标记后允许声称 IRQ 上下文检查有效。`nos_can_block()` 入口在取得对象/任务锁前检查运行状态、current/idle、IRQ 和抢占深度。
- [ ] **Step 5: timer-base 初始化采用受 32 位原子 once 状态保护的发布。** 状态 `UNINITIALIZED/INITIALIZING/READY`；初始化清零所有 base，再 release 发布 READY，其他 CPU acquire 观察。UP 早期初始化在本地 IRQ 关闭区完成，不允许同 CPU IRQ 进入初始化等待环。测试同时让两个 CPU 第一次访问同一个 base，断言初始化只发生一次、两个已注册事件都保留。
- [ ] **Step 6: 运行 cpu suite、现有 spinlock/timer/sched runners，然后提交。** `git commit -m 'CPU: Make preemption and IRQ context CPU local'`。

## Task 3: 统一 timeout 和可同步读取的单调 tick

**Files:** Create `include/nos/timeout.h`, `kernel/ipc/timeout.c`, `tests/host/test_ipc_timeout.c`; modify `kernel/cpu/cpu.c`, `kernel/CMakeLists.txt`.

- [ ] **Step 1: 写边界测试并跑 timeout suite，确认 RED。**

```c
struct nos_deadline d;
assert(nos_deadline_from_ticks(NOS_NO_WAIT, 100U, &d) == 0);
assert(d.no_wait && !d.finite);
assert(nos_deadline_from_ticks(NOS_WAIT_FOREVER, 100U, &d) == 0);
assert(!d.no_wait && !d.finite);
assert(nos_deadline_from_ticks(3U, 100U, &d) == 0);
assert(d.finite && d.tick == 103U);
assert(!nos_deadline_expired(&d, 102U));
assert(nos_deadline_expired(&d, 103U));
assert(nos_deadline_from_ticks(2U, UINT64_MAX - 1U, &d) == -EOVERFLOW);
```

- [ ] **Step 2: 实现完整转换函数。**

```c
int nos_deadline_from_ticks(nos_timeout_t timeout, uint64_t now,
                            struct nos_deadline *deadline)
{
    if (deadline == NULL) {
        return -EINVAL;
    }
    deadline->tick = 0U;
    deadline->finite = false;
    deadline->no_wait = timeout == NOS_NO_WAIT;
    if (timeout == NOS_NO_WAIT || timeout == NOS_WAIT_FOREVER) {
        return 0;
    }
    if (UINT64_MAX - now < timeout) {
        return -EOVERFLOW;
    }
    deadline->finite = true;
    deadline->tick = now + timeout;
    return 0;
}
bool nos_deadline_expired(const struct nos_deadline *deadline, uint64_t now)
{
    return deadline->finite && now >= deadline->tick;
}
```

- [ ] **Step 3: 使 `cpu_run_ticks()` 与 heartbeat 写入同步。** 使用一把只保护 64 位 tick 的 IRQ 自旋锁，不与 rq/base 锁互相嵌套；CPU0 是固件全局 tick 的写入者，host 使用同步逻辑时钟。避免 32 位目标裸读撕裂；不加入 64 位原子内建调用。保留旧运行时计数 API。
- [ ] **Step 4: 跑 `python3 tests/host/run_ipc_test.py timeout --cpus 1` 及 `--cpus 2`，验证 deadline 不因重试重置、tick 在并发读写中单调，然后提交 `IPC: Add explicit timeout and deadline contracts`。**

## Task 4: 32 位引用和对象退休

**Files:** Create `include/nos/refcount.h`, `include/nos/object.h`, `kernel/ipc/object.c`, `tests/host/test_ipc_object.c`; modify `kernel/CMakeLists.txt`.

- [ ] **Step 1: 写对象引用/退休测试并确认 object suite RED。** 覆盖 init=1、get/put、0 不复活、UINT32_MAX 不回绕、close 前 retire 返回 `-EBUSY`、owner 不能释放两次、静态对象不 free、最后 put 仅安排退休。

```c
struct nos_refcount ref;
nos_ref_init(&ref);
assert(nos_ref_get(&ref));
assert(nos_ref_read(&ref) == 2U);
assert(!nos_ref_put(&ref));
assert(nos_ref_put(&ref));
assert(!nos_ref_get(&ref));
```

- [ ] **Step 2: 实现 checked CAS 引用循环。** get 拒绝 0/MAX，put 拒绝下溢；成功 get 使用 acquire，最后 put 使用 release/acquire。`nos_ref_put()` 仅报告是否归零，不执行析构。示例完整 get：

```c
bool nos_ref_get(struct nos_refcount *ref)
{
    uint32_t value = __atomic_load_n(&ref->value, __ATOMIC_RELAXED);
    while (value != 0U && value != UINT32_MAX) {
        if (__atomic_compare_exchange_n(&ref->value, &value, value + 1U,
                                         true, __ATOMIC_ACQUIRE,
                                         __ATOMIC_RELAXED)) {
            return true;
        }
    }
    return false;
}
```

- [ ] **Step 3: 实现 object 头部和退休队列。** init 设置锁、refs=1、空 retire_node、closed=false、owner_released=false；retire 在对象锁内校验 closed/owner，再锁外 put。最后 put 将独立 retire_node 加退休队列；nos_object_reap 在任务上下文逐个摘除、解锁后调用 release。STATIC release 只通知存储可回收，DYNAMIC release 由所属原语负责最终 kfree，不对 header 指针盲目 free。
- [ ] **Step 4: 跑双线程 100000 次 get/put 后只剩 owner 的测试、ASan object suite和 `check_layer_deps.py`，提交 `IPC: Add referenced object retirement`。**

## Task 5: 带代数和引用所有权的 timer 事件

**Files:** Create `include/nos/timer_event.h`, `tests/host/test_ipc_timer.c`; modify `include/kernel/timer.h`, `kernel/timer.c`, `tests/host/run_timer_test.py`, `tests/host/test_timer_subsystem.c`.

- [ ] **Step 1: 写“旧事件已摘链，但回调未执行时重装新事件”的失败测试。** 在 base 解锁、回调调用前闸门暂停旧事件；新线程装 tag=2，恢复后旧回调只能收到 tag=1。release 计数最终恰好为两个。

```c
struct nos_timer_ticket detached = {0};
assert(timer_init_tagged(&timer, "epoch", 0U) == 0);
assert(timer_start_tagged(&timer, 10U, &ticket_one) == 0);
assert(timer_cancel_tag(&timer, 2U, &detached) == 0);
assert(timer_pending(&timer));
assert(timer_cancel_tag(&timer, 1U, &detached) == 1);
assert(detached.tag == 1U);
timer_ticket_release(&detached);
assert(!timer_pending(&timer));
```

- [ ] **Step 2: 运行 timer suite 的 list/wheel、UP/SMP 配置，确认缺失 tagged 行为 RED。**
- [ ] **Step 3: 实现 ticket 模式。** timer 内增加 ticket 和模式字段；tagged init 固定 base CPU；arm 成功消费 caller 已持有的 ticket 引用，失败不消费。已排队时 arm 返回 `-EBUSY`，不隐式覆盖引用。普通 start/mod 不得重装 tagged timer，返回 `-EINVAL`；普通 timer 保持兼容。
- [ ] **Step 4: dispatcher 在 base 锁内复制完整 ticket，摘 timer 后将引用转交局部变量，解锁后 callback，再 release。** callback 及 release 后不得读 timer；保留普通周期 timer 逻辑。cancel_tag 只摘匹配的排队事件，向 detached_ticket 转交引用，返回 1；未命中返回 0，错误返回负 errno，调用方锁外 release。

```c
void timer_ticket_release(struct nos_timer_ticket *ticket)
{
    void (*release)(void *) = ticket->release;
    void *argument = ticket->argument;
    ticket->release = NULL;
    ticket->argument = NULL;
    if (release != NULL) {
        release(argument);
    }
}
```

- [ ] **Step 5: 保护多 CPU 读取 `base->next_expires` 的快路。** 使用 32 位版本号保护的快照或 base 锁读取；没有证明 64 位读原子性的平台不能继续裸 `READ_ONCE(uint64_t)`。不改变普通 timer 的期满/周期契约。
- [ ] **Step 6: 运行 `run_timer_test.py` 和新 timer suite 的四配置；确认回调自重装、回调释放对象及旧 cancel 不删新代，提交 `Timer: Add owned tagged deadline events`。**

## Task 6: Task 引用、独立节点和创建初始化

**Files:** Create `include/nos/task_wait.h`, `kernel/task/task_wait.c`, `tests/host/test_ipc_task.c`; modify `include/kernel/task.h`, `kernel/task/task.c`, `kernel/CMakeLists.txt`.

- [ ] **Step 1: 先声明 wait 类型并新增任务字段，写失败的初始化/回收测试。** wait.h 的最终结构使用上面的契约清单，此时只加入类型与初始化，不实现终态行为。已有 host fixture 必须调用真实 nos_task_wait_init，不能 memset 后假设引用自动有效。

```c
assert(nos_task_wait_init(&task, 0U) == 0);
assert(nos_ref_read(&task.refs) == 1U);
assert(task.wait.state == NOS_WAIT_IDLE);
assert(list_empty(&task.wait.wait_node));
assert(list_empty(&task.wait.wake_node));
assert(list_empty(&task.reap_node));
assert(task_get(&task));
task_put(&task);
assert(nos_ref_read(&task.refs) == 1U);
```

- [ ] **Step 2: 跑 task suite，确认生命周期断言 RED；再实现初始化。** refs=1，wait/task 指针绑定，epoch/wake_epoch=0，所有专用节点为空，exiting=false，专用 timer 初始化到有效 CPU。task_create_ex 在发布到任务表前完成此初始化；失败按创建顺序撤销，不能 put 未初始化引用。现有 `test_sched_multi_policy.c`、`test_sched_load_dump.c` 和新 fixture 的手工 task 初始化也必须显式初始化新增字段，不能让未初始化 refs 混进引用测试。
- [ ] **Step 3: task_get 只增已有效引用，task_put 仅减引用，永不直接 free。** reaper 的基础引用独立保留；任务 epoch 在任务锁内检查 UINT64_MAX，再递增到非零值。`reap_node` 替换 close 队列中对 task->list 的复用。
- [ ] **Step 4: 新增完整的 reaper 资格检查。** 在任务锁及源 rq 同步下检查 ZOMBIE、未在 CPU、所有等待/唤醒节点已脱链、无 timer/wake/callback 引用且 refs==1；摘出候选后锁外清理 VM、Capability、栈和 TCB。列表锁不得带入资格检查或析构。
- [ ] **Step 5: 用两个外部引用阻止 reaper，逐个 put 后才释放；记录 cleanup 只调用一次。** 跑 task、原 task-stack/dump runner，提交 `Task: Retain wait participants until reaping is safe`。

## Task 7: 等待队列、排序和唯一终态

**Files:** Create `kernel/ipc/wait.c`, `tests/host/test_ipc_wait.c`; complete `include/nos/wait.h`; modify `kernel/CMakeLists.txt`.

- [ ] **Step 1: 写纯状态机测试并确认 wait suite RED。** fixture 构造真实 task/object/queue，持任务锁→对象锁准备；两个线程在同一对象锁下竞争 SUCCESS 与 TIMEDOUT，终态和 wake_node 只提交一次。

```c
bool won = nos_wait_complete_locked(&task.wait, NOS_WAIT_SUCCEEDED,
                                    0, 99U, &batch);
assert(won);
assert(!nos_wait_complete_locked(&task.wait, NOS_WAIT_TIMEDOUT,
                                 -ETIMEDOUT, 100U, &batch));
assert(task.wait.state == NOS_WAIT_SUCCEEDED);
assert(task.wait.result == 0);
assert(list_empty(&task.wait.wait_node));
```

- [ ] **Step 2: 实现 prepare。** 校验 queue.object、任务无活动等待、非 exiting、epoch 可增长；固定 task/object 引用，复制 deadline/排序快照，按 RT 优先级/同级 FIFO/普通 FIFO 入队。no_wait 返回 `-EAGAIN`，不入队。入队顺序值耗尽时，持对象锁按当前链表顺序重编号，不能让公平性因整数回绕反转。
- [ ] **Step 3: 实现完整的终态裁决函数。** 仅当 state==PENDING 才能变更；请求 SUCCESS 而 deadline 已到时改为 TIMEDOUT 并返回 false，调用方因此不交付资源。其他终态按首先取得对象锁者提交；摘 wait_node、固定 wake 引用、加入 batch。资源赋值在调用方仍持有同一对象锁时完成，不允许先解锁再写数据。queue.changed_locked 可选，在入队、摘队和重排后于对象锁内调用，供 mutex 标记 PI 捐赠变更；默认 NULL。queue.settle 可选，仅在对象锁外、唤醒确认前调用，用于完成原语的派生状态同步，不允许在 changed_locked 内反向取任务锁。
- [ ] **Step 4: 实现 reorder。** 仅由持任务锁→对象锁的优先级变更路径调用，更新 rank 快照后重新插入，保持原到达序号；选择者只读取队列保护的 rank 字段。队列支持多个 queue 共用一个 object 锁。
- [ ] **Step 5: 跑同级 FIFO、RT/普通混排、deadline 边界、close/cancel 竞争、重复 prepare、epoch 耗尽测试，提交 `IPC: Add single-winner wait state machine`。**

## Task 8: 带 epoch 的调度唤醒和源 CPU 重验证

**Files:** Modify `kernel/sched/sched.c`, `include/nos/sched.h`, `kernel/ipc/wait.c`; create `tests/host/test_ipc_sched.c`; update `tests/host/test_sched_multi_policy.c`.

- [ ] **Step 1: 写“出队但 on_cpu 仍为 1”的双 CPU 测试。** 源 CPU 停在上下文保存闸门；另一 CPU 调 sched_wake_epoch，断言只在源 rq 恢复，不在第二 CPU 上执行。

```c
assert(sched_wake_epoch(&task, epoch) == 0);
assert(task.wake_epoch == epoch);
assert(task.cpu == source_cpu);
assert(source_rq->curr == &task || task.se.on_rq);
assert(sched_wake_epoch(&task, epoch) == 0);
assert(source_rq->nr_running == running_after_first_wake);
```

- [ ] **Step 2: 跑 sched suite，确认当前无 epoch 校验/错误迁移/重复入队触发 RED。**
- [ ] **Step 3: 实现 prepare 和 wake。** prepare 只在当前 task 锁下摘源 rq；wake 取得 task 锁校验 epoch/exiting，取得源 rq 锁重新读 on_cpu。仍执行时只恢复源 rq；已离开时释放源 rq，在 task 锁下选目标并单独取目标 rq 入队。源/目标 rq 不同时持有。完成后在锁外发送一次必要的 IPI 请求。
- [ ] **Step 4: 修正上下文交接的测试 HAL 契约。** 同步切换必须在旧上下文实际保存前维持源 rq 保护；host 停在“保存前”时不能让另一 CPU 把任务迁走。Cortex-M 继续使用 PendSV，不能用 context_switch 返回作为完成证明。本任务只测试/接入现有可工作的路径，不补架构空桩。
- [ ] **Step 5: 将全局 `nr_uninterruptible` 更新放回对应 rq 保护内。** 同代重复唤醒和已退出任务为 no-op；旧代不清除新 wchan、timer 或等待状态。
- [ ] **Step 6: 跑 scheduler 既有多策略/load runner及 sched suite，提交 `Sched: Serialize epoch wakeups with CPU ownership`。**

## Task 9: 连接 deadline、wake batch、park 和 finish

**Files:** Modify `kernel/ipc/wait.c`, `kernel/task/task_wait.c`, `include/nos/task_wait.h`; create `tests/host/test_ipc_integration.c`.

- [ ] **Step 1: 写完整等待操作的失败测试。** 先 SUCCESS 暂不 flush，等待者不得返回或复用记录；flush 之后 park 即使尚未真正睡眠也必须返回本代结果。

```c
nos_wake_flush(&batch);
nos_wait_park(&task, epoch);
assert(nos_wait_finish(&task, epoch) == 0);
assert(task.wait.state == NOS_WAIT_IDLE);
assert(task.wait.object == NULL);
assert(nos_ref_read(&object.refs) == 1U);
assert(nos_ref_read(&task.refs) == 1U);
```

- [ ] **Step 2: 运行 integration suite，确认未发布 wake_epoch 时提前返回会失败。**
- [ ] **Step 3: 完成 prepare 的调度与 timer 接线。** 在 task→object 锁下调用 sched_wait_prepare_locked，然后注册 finite tagged timer；arm 出错时同锁内摘等待记录、恢复 rq 和 IDLE，锁外释放引用。先完成 Task 7 的所有输入校验再出队，避免失败后缺少 runnable 任务。
- [ ] **Step 4: 实现 nos_wait_timeout。** task 引用由 ticket 持有；取得 task 锁核对 epoch 和 managed 身份，再取 object 锁提交 TIMEDOUT，解锁后 flush。非本代直接返回，ticket dispatcher 统一 put，不在 callback 再 put 第二次。
- [ ] **Step 5: 实现 flush、park 和 finish。** flush 摘 wake_node，局部保存 task/epoch/object/settle，固定局部对象引用，cancel_tag 并锁外 release；调用可选 settle 后再调用 sched_wake_epoch，最后 put 局部引用。发布后不再处理记录字段。park 只根据本代 wake_epoch 决定是否仍需 switch。finish 在 task→object 锁下读结果并清活动身份，锁外 put；未确认同代唤醒时返回 `-EAGAIN`，不得清空记录。
- [ ] **Step 6: 强制执行 arm 失败回滚、callback 已派发后重装、close/cancel/timeout 三方交错和 CPU 迁移测试。** 运行 UP/SMP list/wheel 四配置，提交 `IPC: Connect wait completion to deadlines and scheduling`。

## Task 10: 旧 IPC 的引用、代数和唤醒兼容层

**Files:** Modify `kernel/task/task_wait.c`, `kernel/task/task.c`, `kernel/ipc/sem.c`, `kernel/ipc/msg_queue.c`, `kernel/ipc/endpoint.c`, `kernel/ipc/notification.c`; create `tests/host/test_ipc_legacy.c`.

- [ ] **Step 1: 写“旧 timer 回调晚于新的 managed wait”和“legacy wake_node 已派发后重新入队”的失败测试。** legacy callback 不可设置新 wait 的 flag，旧票据不可取消新 timer；每批次 task 引用恰好释放一次。
- [ ] **Step 2: 运行 legacy suite RED。** 用真实旧原语源码，不能只测试一个与旧路径无关的假对象。
- [ ] **Step 3: 在 task_block_lock 为 legacy 分配新 epoch。** legacy 和 managed 共用 `task.wait_epoch` 的单调分配器，legacy_epoch 只保存本次快照，不能独立从 1 开始造成代数碰撞。旧 timeout arm 统一改为 task_legacy_timeout_start_locked；wrapper 捕获 epoch 并取得 task 引用，以 tagged dispatch 调用旧 timeout 逻辑，进入旧逻辑前在 task 锁下验证身份/代数。新等待 arm 失败要回滚阻塞和链表，不忽略 timer 返回码。
- [ ] **Step 4: 旧释放路径在原对象锁内快照 task/epoch 并 enqueue。** 以独立的受锁保护 legacy_wake_node 合并同代请求；新代可取代尚未派发的旧代。dispatcher 摘节点、转移引用到局部票据后解锁，验证 epoch 再调度；不复用 task->list，不在对象锁内取得 task 锁。队列 flush 在返回/IRQ 返回前完成，只请求调度。
- [ ] **Step 5: 替换消息队列 owner 直接 resume，以及 sem/notification/endpoint 临时 task->list 唤醒链表。** 等待字段只在原对象锁或 task 身份验证规则下访问。保留旧公开 API 和资源语义；Endpoint 已交付 RPC 关闭遗漏、Capability 裸指针仍是第三阶段遗留，不能在本提交宣称修复。
- [ ] **Step 6: task_resume 对活动 managed wait 返回 `-EBUSY`；legacy 重复 ready/wake 返回 0。** 取消旧 timer 必须使用被快照 epoch，不能让晚到的旧 resume 停止新事件。
- [ ] **Step 7: 跑 legacy/integration suite及 USB/UART/shell/VFS/FatFs 现有 runner，提交 `IPC: Protect legacy timeout and wake participants`。**

## Task 11: Mutex 获取、ownership 交接和静态初始化

**Files:** Modify `include/kernel/mutex.h`, `kernel/ipc/mutex.c`, `kernel/task/task.c`; create `tests/host/test_ipc_mutex.c`; update `tests/host/test_mutex.c`, `tests/host/run_mutex_test.py`, `tests/host/mutex_stubs/kernel/mutex.h`, `tests/host/mutex_stubs/kernel/task.h`。

- [ ] **Step 1: 写真实 mutex 的失败测试并跑 mutex suite RED。** 覆盖 NO_WAIT/FOREVER、递归、有限 deadline、无竞争零 switch、handoff-vs-timeout、静态 MUTEX 不调用 init 即可使用。

```c
assert(mutex_acquire(&lock, NOS_NO_WAIT) == 0);
assert(lock.owner == &owner);
assert(mutex_acquire(&lock, NOS_NO_WAIT) == -EDEADLK);
mutex_unlock(&lock);
assert(!mutex_is_locked(&lock));
assert(context_switch_count == switches_before);
```

- [ ] **Step 2: mutex 内嵌 nos_object 和 nos_wait_queue。** 修改所有生产代码中对 mutex.lock/wait_list 的直接访问；静态宏初始化 object.lock、refs=1、队列、held_node、retire_node、owner 和版本号。kernel/task/task.c 的 PI 代码必须同步编译通过。
- [ ] **Step 3: 实现 mutex_acquire 的立即和等待路径。** 在入口计算 deadline/context，任务锁→object 锁尝试，owner==current 返回 -EDEADLK；失败时调用 prepare，锁外 park/finish。立即成功在 owner 的 task 锁下挂 held_node。boot 仅 NO_WAIT/try 可用，不能把当前任务为空等同于持锁成功。
- [ ] **Step 4: 实现交接的“快照→按 ID 取两任务锁→重验证”。** 对象锁内固定候选引用/版本，解锁后按 pid（任务引用防止 PID 复用）排序取得旧 owner 和候选 task 锁，再取 object 锁；owner/候选/版本不匹配就解锁重试。成功时同锁内将 held_node 转移并提交 SUCCESS，再锁外 flush。
- [ ] **Step 5: 保留兼容包装。**

```c
int mutex_lock_timeout(struct mutex *lock, uint32_t ticks)
{
    return mutex_acquire(lock, ticks == 0U ? NOS_WAIT_FOREVER : ticks);
}
void mutex_lock(struct mutex *lock)
{
    int rc = mutex_acquire(lock, NOS_WAIT_FOREVER);
    if (rc < 0) {
        pr_err("mutex lock failed: %d\n", rc);
    }
}
```

`mutex_try_lock()` 保持既有成功/失败返回约定，通过新的 NO_WAIT 获取路径转换；错误 owner unlock 保留可诊断拒绝行为。

- [ ] **Step 6: 同一测试分别强制超时先提交和交接先提交。** 断言 owner 与返回结果一致，held_node 只在一个任务链表，0 个额外 task/object 引用；提交 `Mutex: Use unified wait and ownership handoff`。

## Task 12: 有界 PI、来源聚合和排序更新

**Files:** Create `include/nos/mutex_pi.h`, `kernel/ipc/mutex_pi.c`, `tests/host/test_ipc_pi.c`; modify `kernel/ipc/mutex.c`, `kernel/task/task.c`, `include/kernel/task.h`, `kernel/CMakeLists.txt`.

- [ ] **Step 1: 写两个独立捐赠来源和旧计算覆盖新捐赠的失败测试。** 保留当前 4 跳链测试，再增加 waiter 超时、动态改优先级、handoff、新捐赠同时到达的交错。

```c
assert(owner.current_priority == high_waiter.current_priority);
assert(task.wait.state == NOS_WAIT_TIMEDOUT);
assert(owner.current_priority == remaining_waiter.current_priority);
assert(owner.ipc_boost_prio == ipc_priority_before);
assert(owner.current_priority <= owner.init_priority);
```

这里 `task` 是已经超时的最高优先级等待者；`remaining_waiter` 表示仍有效的另一个来源，fixture 在断言前完成所有传播。

- [ ] **Step 2: 跑 pi suite，确认单个 pi_boost_prio 清空其他来源的旧行为 RED。**
- [ ] **Step 3: 分开存储 mutex_boost_prio 和 ipc_boost_prio。** `task_pi_boost/unboost` 保留旧 Endpoint 输入为 IPC 来源；mutex 通过 mutex_pi_recompute 更新自身来源，最终有效优先级为 init、mutex、IPC 三者最小值。不得再从未固定的 ipc_partner 裸指针推算来源；旧 Endpoint 调用处显式提供已知捐赠值并在对应完成时撤销。此处保留单来源兼容能力，不宣称旧 Endpoint 的多请求捐赠或超时撤销已经完成；第三阶段以请求事务替换。
- [ ] **Step 4: 在队列增删/排序变更下增加 owner 的 32 位 donation_seq。** mutex 将 queue.changed_locked 绑定到对象锁内序号更新，将 queue.settle 绑定到锁外 owner 快照/引用/传播。这样通用 timeout/cancel 摘除等待者时也会撤销 PI，不依赖已超时任务重新运行。recompute 持任务锁稳定 held_mutex_list，逐个取 object 锁读 rank；提交前验证序号，变化则重算。变更方解锁对象后调用传播，不在对象锁内获取 owner task 锁。
- [ ] **Step 5: 传播最多 MUTEX_PI_MAX_DEPTH=4。** 每一步固定 owner/下一对象引用，释放当前对象锁后取目标 task 锁、重验证版本；不跨两个 object 锁递归。task_set_prio/policy 改变等待 rank 时使用 task→object 顺序，并通知等待对象的 PI 来源。结束后锁外请求必要 reschedule。
- [ ] **Step 6: 跑 pi/mutex/integration suite 及既有多策略调度回归，提交 `Mutex: Recompute bounded PI without dropping other donors`。**

## Task 13: Task 取消、退出限制及安全退休闭环

**Files:** Modify `kernel/task/task.c`, `kernel/task/task_wait.c`, `include/kernel/task.h`, `kernel/ipc/wait.c`; create `tests/host/test_ipc_cancel.c`.

- [ ] **Step 1: 写取消/成功交错及持锁退出的失败测试。**

```c
assert(task_cancel(&owner) == -EBUSY);
assert(owner.status != TASK_ZOMBIE);
assert(task_cancel(&waiter) == 0);
assert(waiter.exiting);
assert(waiter.status == TASK_ZOMBIE);
assert(!nos_task_reapable_locked(&waiter));
```

最后一个断言的 fixture 仍持有一个已派发 timer ticket 引用。

- [ ] **Step 2: 跑 cancel suite RED。**
- [ ] **Step 3: 实现 task_cancel。** task 锁内拒绝 held_mutex_list 非空，标记 exiting；managed 等待按 task→object 提交 CANCELLED，已 SUCCESS 不改写。取消 queued timer 归还 ticket 引用，已派发 ticket 自行归还。摘 rq 后登记 ZOMBIE，锁外 flush 和登记 reap_node，不等待回调结束。
- [ ] **Step 4: 对接 task_del/正常退出。** task_del 调 task_cancel 并记录拒绝；task_exit/task_exit_with_code 检测持锁退出并触发诊断。managed wake/legacy wake 对 exiting 任务只消费票据、不重新 runnable。外部跨任务取消调用必须持 task 引用，不能拿 task_find_by_pid 的裸指针在解锁后直接取消。
- [ ] **Step 5: 为不会再返回用户代码的取消任务实现 nos_wait_abandon。** 条件为 exiting/ZOMBIE、源 rq 已确认 off-CPU、本代终态、wake_node 已摘且 wake_epoch 已确认。按 task→object 锁清活动记录，锁外释放等待操作自身的 task/object 引用；未满足返回 `-EAGAIN`，无活动记录返回 0。sched_wake_epoch 对 exiting 任务仍消费本代确认，但不 enqueue。否则一个已经取消且再也不执行 finish 的任务会永远被自己的等待引用固定。
- [ ] **Step 6: reaper 先尝试 abandon，再检查最终引用资格。** 对未完成上下文保存的旧 task 不释放栈。cleanup 中可以创建/销毁另一个对象，证明不带 task-list/retire 锁进入析构。加入“等待任务被取消且永不恢复执行，最终仍能回收”的测试。
- [ ] **Step 7: 跑 cancel/task/legacy/integration suite，提交 `Task: Cancel waits before referenced teardown`。**

## Task 14: 配置矩阵、成本报告和阶段验收

**Files:** Create `tests/host/test_ipc_stress.c`, `tests/host/report_ipc_cost.py`, `docs/superpowers/reviews/2026-11-02-ipc-wait-foundation-verification.md`; modify `tests/run_host_regressions.py`, `.gitlab-ci.yml`, workflow notes。

- [ ] **Step 1: 添加可重复压力测试。** 记录随机种子，两个以上 host CPU 并发执行 acquire/unlock、超时、取消、close 和 legacy/managed 交替；每种操作最终断言引用回归基线、队列无重复节点、成功计数和 owner 一致。runner 使用固定种子矩阵，单组最长 120 秒，不依赖 sleep 制造交错。
- [ ] **Step 2: 执行全部 IPC suite 的 UP/SMP、list/wheel 矩阵。**

```sh
python3 tests/host/run_ipc_test.py all --cpus 1
python3 tests/host/run_ipc_test.py all --cpus 1 --small-memory
python3 tests/host/run_ipc_test.py all --cpus 4
python3 tests/host/run_ipc_test.py all --cpus 4 --small-memory
python3 tests/host/run_ipc_test.py all --cpus 4 --sanitize address,undefined
python3 tests/host/run_ipc_test.py all --cpus 4 --sanitize thread
```

runner 同时支持 `all`，等价按 Task 1 的 suite 顺序执行；sanitizer 先编译/运行空程序探测 runtime，工具不可用标注 SKIP 及证据，测试报告不得写成 PASS。

- [ ] **Step 3: 执行仓库回归与板级构建。** 各 configure/build 配对顺序执行，独立板型可并行；保留每个返回码和 warning 日志。

```sh
python3 tests/run_host_regressions.py
python3 -m unittest discover -s tests -p 'test_*.py'
cmake -P tests/test_arch_flags.cmake
cmake -P tests/test_soc_headers.cmake
python3 scripts/check_layer_deps.py
cmake --preset nk60-v2
cmake --build --preset nk60-v2
cmake --preset qemu-stm32
cmake --build --preset qemu-stm32
cmake --preset wl-001
cmake --build --preset wl-001
cmake --preset zj-v3
cmake --build --preset zj-v3
python3 tests/qemu_stm32_smoke.py build/qemu-stm32/nos.elf
```

- [ ] **Step 4: 生成真实布局及固件成本报告。** host sizeof 记录 UP/SMP；ARM 用一个无运行依赖 probe 对象生成符号 `char nos_size_task[sizeof(struct task_struct)]`、`nos_size_wait`、`nos_size_timer`，用 arm-none-eabi-nm 的符号大小读取 32 位布局。以 spec 基线 `0fdc410` 的独立临时 checkout 构建相同四 preset，对比 `arm-none-eabi-size` 的 text/data/bss；基线和新版本使用同工具链及配置。不得用 host 64 位 sizeof 充当 MCU 成本。

```c
const unsigned char nos_size_task[sizeof(struct task_struct)] = {0};
const unsigned char nos_size_wait[sizeof(struct nos_wait_context)] = {0};
const unsigned char nos_size_timer[sizeof(struct timer)] = {0};
_Static_assert(sizeof(struct nos_ipc_msg) == 64U, "IPC ABI changed");
```

- [ ] **Step 5: 检查四 ELF 无未解析 atomic/helper 引用。** 对各 ELF 执行 arm-none-eabi-nm -u；不允许新出现 `__atomic_*_8`、`__sync_*_8` 或未解析的 task/wait 接口。编译 probe 使用相应 build/include/autocfg.h 和实际架构 include。
- [ ] **Step 6: 写验收报告。** 列出每个测试、配置、命令/退出码、sanitizer 状态、成本表和未迁移事项；分别记录“底座/mutex 已验证”“旧 sem/msgq/notification 和 Endpoint 资源协议待第二/三阶段”“真实多核启动/IPI/上下文/缓存未验收”。删除任何仍声称硬件多核已完成的说明。
- [ ] **Step 7: 使用 superpowers:requesting-code-review 审查相对基线的实际差异。** 修复实质问题后仅重跑受影响测试及必要构建；任何新同步变化必须增加对应交错测试。
- [ ] **Step 8: 提交验收与 CI 接线。** `git commit -m 'Tests: Verify IPC wait lifecycle and mutex concurrency'`；再按 finishing-a-development-branch 提供合并/PR/保留工作树选项，不默认部署或烧录设备。

## 规范覆盖自查

| 规范要求 | 实施任务 |
| --- | --- |
| CPU 本地深度、IRQ 和 can_block | 2 |
| timeout 约定、单调时钟、绝对预算和溢出 | 3 |
| 对象关闭/退休、32 位引用与存储类型 | 4、13 |
| tagged 派发、旧 cancel 不影响新代 | 5、9、10 |
| 任务引用、独立节点、回收栈/TCB | 6、10、13 |
| 唯一终态、deadline 优先和等待排序 | 7 |
| 源 rq/on_cpu 握手、无重复入队、IPI 请求契约 | 8 |
| prepare/park/flush/finish、新旧代隔离 | 9 |
| 旧 IPC 最小引用/节点适配及 managed 保护 | 10 |
| owner/held_node 原子交接、静态初始化和兼容 API | 11 |
| 4 跳 PI、来源聚合、序号重验证、优先级排序 | 12 |
| 取消、持 mutex 不可强删、退出不复活 | 13 |
| 真实原语主机并发、sanitizer、四板/QEMU、ABI/成本 | 1、14 |

## 执行交接

计划审核只检查是否覆盖已批准规范、签名一致、命令/路径可定位、无未定义步骤；不把计划中的测试示例当作已经运行的结果。

可选执行方式：子代理逐任务实施并在每个任务后审查，或当前会话按序实施。两种方式都先建立独立工作树；源码改动前执行对应 TDD 步骤。执行代理应将每一步实际命令和结果记录到 `.planning/2026-11-02-ipc-redesign/progress.md`，不得仅把复选框勾选视为验证。
