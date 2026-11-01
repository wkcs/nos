# NOS 调度器与任务管理子系统重构设计规范（多调度类并存 + Per-CPU SMP + PELT 负载跟踪 + 小内存友好栈回溯与 Dump）

- **日期**：2026-11-01
- **模块**：
  - 调度核心与多调度类：`include/nos/sched.h`, `include/kernel/sched.h`, `kernel/sched/sched_core.c`, `kernel/sched/sched_rt.c`, `kernel/sched/sched_fair.c`, `kernel/sched/sched_idle.c`, `kernel/sched/sched_load.c`
  - 任务管理与同步对接：`include/kernel/task.h`, `kernel/task/task.c`, `kernel/ipc/mutex.c`, `kernel/ipc/sem.c`, `kernel/ipc/msg_queue.c`, `kernel/ipc/endpoint.c`, `kernel/ipc/notification.c`
  - 栈回溯、异常转储与可观测性：`include/kernel/unwind.h`, `kernel/task/task_dump.c`, `kernel/kallsyms.c`, `kernel/panic.c`, `arch/arm/cpu/cpu.c`, `arch/arm/cpu/cortex-m3/cpu_c.c`, `arch/arm/cpu/cortex-m4/cpu_c.c`, `fs/procfs.c`, `app/shell/nshell.c`

---

## 1. 背景与现状痛点诊断

当前 NOS 的调度器 (`kernel/sched/sched.c`) 与任务管理 (`kernel/task/task.c`) 在从小内存 MCU（如 `wl-001` STM32F103，仅 20KB SRAM / 128KB Flash）向多核处理器（ARM64 / RISC-V64 / x86_64）演进过程中暴露出 6 个核心问题：

1. **单核全局状态硬编码，缺乏 Per-CPU 运行队列 (`rq`)**：
   - 全局仅有单份 `g_current_task`、`ready_task_list[CONFIG_MAX_PRIORITY]`、`ready_list_lock`、`g_switch_lock` 与 `scheduler_lock_nest`。
   - 无法表达多核 SMP 下每个 CPU 核心独立运行的任务、独立就绪队列锁以及跨核唤醒 IPI (`hal_smp_send_ipi`)。
2. **单一固定优先级轮转，缺乏多调度策略/调度类 (`sched_class`) 并存能力**：
   - 所有任务混用同一个固定优先级数组，缺少 Linux 风格的 `sched_class`（`rt_sched_class` + `fair_sched_class` + `idle_sched_class`）分层职责链。
   - 既无法为普通交互/计算任务提供基于虚拟运行时间 (`vruntime`) 的按权重公平调度（CFS/EEVDF），也缺少按策略动态切换能力。
3. **抢占控制与延迟上下文切换竞态**：
   - Cortex-M 的 `context_switch()` 仅置位异步 `PendSV` 异常便返回，但 `switch_task()` 在 `PendSV` 真正触发前即修改了 `g_current_task` 和任务状态。
   - `sch_lock()` / `sch_unlock()` 仅修改 `scheduler_lock_nest`，而 `switch_task()` 根本未检查该计数，导致关调度期间主动调用 `switch_task()` 仍会切换，丢失重调度请求。
4. **任务状态机语义混淆、链表节点复用与多锁优先级继承缺陷**：
   - `del_task_to_ready_list()` 无条件置为 `TASK_SUSPEND`，随后 `task_hang()` 改为 `TASK_WAIT`，导致挂起 (`SUSPENDED`) 与阻塞等待 (`SLEEPING`/`BLOCKED`) 混淆。
   - `task->list` 同时复用于就绪队列、等待队列和死亡回收队列，超时定时器与信号量唤醒并发时存在链表踩踏风险。
   - 优先级继承仅保存单一 `init_priority`，持有多个 Mutex/Endpoint 时释放其一即退回初始优先级。
   - `task_foreach()` 遍历全局任务链表未加锁，缺少 `ppid`、`exit_code`、`wchan`（阻塞通道）记录。
5. **CPU 负载跟踪 (Load Tracking) 跨秒清零失真**：
   - 现有 `calculate_task_time()` 依赖 1 秒跳变的 `g_sys_cycle` 清零式统计，任务跨秒休眠或未切换时 `task_get_cpu_usage()` 直接归零；缺少定点 PELT 平滑负载与系统级 1m/5m/15m `loadavg`。
6. **栈回溯 (Backtrace)、Crash Dump 与 `ps` 可观测性薄弱**：
   - `arch_dump_stack()` 仅能回溯当前任务，假设旧式 APCS 帧且硬编码 `0x20000000~0x30000000`，不支持对任意阻塞任务 (`dump_task_stack`) 回溯，也未考虑小内存 MCU（如 `wl-001` 仅 1KB 任务栈、`-O2` 无帧指针）的资源约束。
   - `kallsyms_lookup()` 为线性扫描且无符号大小计算；`ps` 与 `/proc/tasks` 缺少 `%CPU`、`POL`、`TIME`、`CSW`、`WCHAN`、`Load Avg` 及 `ps -l` / `ps -s <pid>` 多视图诊断。

---

## 2. 总体架构设计

借鉴 **Linux 内核**（Per-CPU `rq` + `sched_class` 职责链 + CFS/EEVDF `vruntime` + PELT/LoadAvg + `wchan`/Unwinder）与 **Zephyr / RT-Thread**（MCU 零膨胀静态裁剪、O(1) 位图队列、Thumb-2 指令启发式栈扫描），构建 **“Per-CPU 运行队列 + 多调度类并存 + 定点 PELT 负载跟踪 + 零堆分配双模栈回溯”** 四层架构：

```text
+----------------------------------------------------------------------------------------+
| 统一任务与调度公共接口层 (include/kernel/task.h, include/kernel/sched.h, include/nos/sched.h)|
| - 任务生命周期: task_create / task_create_ex / task_ready / task_del / task_waitpid     |
| - 阻塞与同步:   task_sleep / task_block_on / task_wakeup / task_hang / task_resume      |
| - 策略与亲和性: task_set_prio / task_set_policy / task_set_nice / task_set_affinity     |
| - 诊断与回溯:   dump_stack / dump_task_stack / dump_all_task_ex / sched_get_loadavg     |
+-------------------------------------------+--------------------------------------------+
                                            |
                                            v
+----------------------------------------------------------------------------------------+
| 调度主干与 Per-CPU 运行队列层 (kernel/sched/sched_core.c & sched_load.c)                |
| - Per-CPU struct sched_rq (g_sched_rqs[CONFIG_NR_CPUS]; 单核编译期折叠为单实例)         |
| - 抢占与重调度: preempt_count + need_resched 延迟重调度 + 跨核 NOS_IPI_RESCHED          |
| - SMP 选核与均衡: select_task_rq (亲和性 + 最闲核) + Tick/Idle Work Stealing            |
| - 负载跟踪引擎: 1024 标度定点 PELT (util_avg) + 全局 1m/5m/15m 定点 avenrun            |
+-------------------+-------------------------------+------------------------------------+
                    |                               |
                    v                               v
+-----------------------------------+ +-----------------------------------+ +------------+
| 实时调度类 (rt_sched_class)       | | 公平调度类 (fair_sched_class)     | | 空闲调度类 |
| (kernel/sched/sched_rt.c)         | | (kernel/sched/sched_fair.c)       | | (sched_idle|
| - O(1) 优先级位图 (32 / 256 级)   | | - 64位虚拟运行时间 vruntime       | |  .c)       |
| - SCHED_FIFO & SCHED_RR 时间片    | | - Nice (-20..+19) 权重比例分配    | | - Per-CPU  |
| - 100% 兼容现有 RTOS 优先级语义   | | - 睡眠唤醒补偿防饥饿与防垄断      | |   Idle 与  |
| - SMP RT Push/Pull 跨核迁移       | | - Kconfig 可裁剪 (MCU 可关或开)   | |   僵尸回收 |
+-----------------------------------+ +-----------------------------------+ +------------+
```

---

## 3. 核心数据结构与状态机设计

### 3.1 任务状态机与专用链表隔离 (`include/kernel/task.h`)

1. **清晰的 6 态枚举（保留旧名宏别名，确保全仓零破坏兼容）**：
   ```c
   enum task_status {
       TASK_RUNNING   = 0x01, /* 正在 CPU 核心上运行 */
       TASK_READY     = 0x02, /* 位于 sched_rq 就绪队列等待调度 */
       TASK_SLEEPING  = 0x04, /* 可中断睡眠/等待 (sleep / sem / msg_q / notify) */
       TASK_SUSPENDED = 0x08, /* 被显式挂起 (task_hang / SIGSTOP)，不响应普通唤醒 */
       TASK_ZOMBIE    = 0x10, /* 已退出待回收 (原 TASK_CLOSE) */
       TASK_BLOCKED   = 0x20, /* 不可中断深度阻塞 (mutex / synchronous endpoint IPC) */
   };
   #define TASK_RUNING  TASK_RUNNING
   #define TASK_WAIT    TASK_SLEEPING
   #define TASK_SUSPEND TASK_SUSPENDED
   #define TASK_CLOSE   TASK_ZOMBIE
   ```
2. **调度策略枚举 (`enum sched_policy`)**：
   ```c
   enum sched_policy {
       SCHED_POLICY_RR     = 0, /* 实时时间片轮转（默认，兼容现有 task_create） */
       SCHED_POLICY_FIFO   = 1, /* 实时先进先出（无时间片抢占，同优先级运行至让出） */
       SCHED_POLICY_NORMAL = 2, /* 公平调度 CFS/EEVDF（按 nice 权重分配 vruntime） */
       SCHED_POLICY_IDLE   = 3, /* Per-CPU 空闲回收任务专属策略 */
   };
   ```
3. **链表节点物理隔离（消除复用踩踏）**：
   - `task->se.run_node`：**仅用于挂入 `sched_rq` 的就绪队列**（`rt_rq` 或 `fair_rq`），由 `rq->lock` 保护；
   - `task->list` + `task->list_lock`：**仅用于挂入同步原语等待队列**（`mutex` / `sem` / `msg_queue` / `endpoint` / `notification`）或退出回收队列，100% 兼容现有 IPC 代码；
   - `task->tlist`：**仅用于挂入全局任务链表 `g_task_list`**，由 `g_task_list_lock` 保护。

### 3.2 内存紧凑型 `struct sched_entity` 与 `struct task_struct`

针对 `wl-001`（20KB SRAM）等小内存 MCU，**移除旧版冗余的 24 字节周期统计字段**（`run_time`, `sys_cycle`, `save_run_time`, `save_sys_cycle`），原位替换为紧凑的 `struct sched_entity`，使每个 TCB 的净增内存控制在 **16~24 字节以内**：

```c
struct sched_entity {
    struct list_head run_node;      /* 调度器就绪队列专用节点 */
    uint64_t         vruntime;      /* 虚拟运行时间 (ns/us，Fair 调度使用) */
    uint64_t         sum_exec_us;   /* 累计总执行微秒数 (永不清零) */
    uint64_t         exec_start_us; /* 本次投入运行的时间戳 (us) */
    uint32_t         win_start_us;  /* 当前 PELT 窗口起始时间低 32 位 (us) */
    uint32_t         win_run_us;    /* 当前 PELT 窗口累计运行时间 (us) */
    uint16_t         util_avg;      /* 定点 PELT 负载均值 (0..1024) */
    uint16_t         weight;        /* 调度权重 (由 nice 值映射，nice 0 = 1024) */
    uint32_t         nvcsw;         /* 主动上下文切换次数 (Voluntary CSW) */
    uint32_t         nivcsw;        /* 被动抢占上下文切换次数 (Involuntary CSW) */
    bool             on_rq;         /* 是否处于就绪队列中 */
};
```

在 `struct task_struct` 中补充以下紧凑管理字段：
- `const struct sched_class *sched_class;`
- `struct sched_entity se;`
- `pid_t ppid;`（父任务 PID，创建时自动继承 `current ? current->pid : 0`）
- `int16_t exit_code;`（退出状态码）
- `int8_t nice;`（`-20 .. +19`，默认 `0`）
- `uint8_t policy;`（`enum sched_policy`）
- `uint8_t cpu;`（当前或最近运行的 CPU 核号）
- `uint8_t on_cpu;`（当前是否正在 CPU 上执行）
- `uint8_t cpus_allowed;`（CPU 亲和性位图，支持最多 8 核，默认 `0xFF`）
- `uint8_t pi_boost_prio;`（多锁优先级继承的最高借用优先级，`0xFF` 表示无继承）
- `uintptr_t wchan_addr;`（当前阻塞等待的内核对象或调用点 PC 地址）
- `const char *wchan_name;`（阻塞原因简短标签，如 `"sleep"`, `"mutex"`, `"sem"`, `"msgq"`, `"ipc"`, `"notify"`）

### 3.3 多锁优先级继承恢复机制

现有 `mutex_unlock()` 与 `endpoint.c` 直接调用 `task_set_prio(owner, owner->init_priority)`，在嵌套持锁下会错误丢失优先级。本次重构引入：
- `task_pi_boost(struct task_struct *owner, uint8_t waiter_prio)`：当高优先级任务阻塞在 `owner` 持有的锁/端点上时，提升 `owner->pi_boost_prio = min(owner->pi_boost_prio, waiter_prio)` 并重算有效优先级 `effective_prio = min(owner->init_priority, owner->pi_boost_prio)`；
- `task_pi_unboost(struct task_struct *owner, uint8_t next_highest_waiter_prio)`：释放锁时传入剩余等待者的最高优先级（无等待者传 `0xFF`），安全恢复至 `init_priority` 或次高继承优先级；
- `task_set_prio(task, prio)`：更新 `task->init_priority` 与有效优先级，保持与所有现有调用方的完全兼容。

---

## 4. 多调度类 (`sched_class`) 与 Per-CPU SMP 运行队列设计

### 4.1 Per-CPU 运行队列 (`struct sched_rq`)

```c
#ifndef CONFIG_NR_CPUS
#ifdef CONFIG_SMP
#define CONFIG_NR_CPUS 4
#else
#define CONFIG_NR_CPUS 1
#endif
#endif

struct rt_rq {
    struct list_head queue[CONFIG_MAX_PRIORITY];
    uint32_t         prio_grp;
#if CONFIG_MAX_PRIORITY > 32
    uint8_t          prio_tbl[32];
#endif
    uint32_t         nr_running;
    uint8_t          highest_prio;
};

struct fair_rq {
    struct list_head tasks_timeline; /* 按 vruntime 升序排列的就绪链表 */
    uint64_t         min_vruntime;
    uint32_t         nr_running;
    uint32_t         load_weight;
};

struct sched_rq {
    spinlock_t          lock;
    uint32_t            cpu_id;
    uint32_t            nr_running;
    uint32_t            nr_uninterruptible;
    uint64_t            nr_switches;
    uint32_t            preempt_count;
    bool                need_resched;

    struct task_struct *curr;
    struct task_struct *idle;

    struct rt_rq        rt;
    struct fair_rq      fair;

    /* Per-CPU PELT 与时间记账 */
    uint64_t            clock_us;
    uint64_t            busy_time_us;
    uint64_t            idle_time_us;
    uint32_t            win_start_us;
    uint32_t            win_busy_us;
    uint16_t            cpu_util_avg; /* 0..1024 */
};
```

- 在 `CONFIG_SMP=n`（如 STM32 MCU）下，`CONFIG_NR_CPUS == 1`，`this_rq()` 直接内联展开为 `&g_sched_rqs[0]`，无任何多核查表开销。
- 在 `CONFIG_SMP=y` 下，`this_rq()` 通过 `hal_cpu_id()` 索引当前核 `sched_rq`。

### 4.2 三大调度类行为规范

1. **`rt_sched_class` (`kernel/sched/sched_rt.c`)**：
   - 管理 `SCHED_POLICY_RR` 与 `SCHED_POLICY_FIFO` 任务；
   - `enqueue_task` / `dequeue_task`：通过 `task->se.run_node` 插入/移除 `rq->rt.queue[prio]`，维护 O(1) 位图与 `highest_prio`；
   - `pick_next_task`：利用 `__ffs()` O(1) 定位最高优先级非空队列首元素；
   - `task_tick`：`SCHED_POLICY_FIFO` 不扣减时间片；`SCHED_POLICY_RR` 递减 `remaining_tick`，归零时重填 `init_tick`、若同优先级队列有多于 1 个任务则移至队尾并置位 `rq->need_resched = true`；
   - `check_preempt_curr`：新唤醒任务若为 RT 且 `p->current_priority < rq->curr->current_priority`（或 `rq->curr` 为 Fair/Idle 任务），立即置位 `rq->need_resched = true`。
2. **`fair_sched_class` (`kernel/sched/sched_fair.c`)**：
   - 管理 `SCHED_POLICY_NORMAL` 任务；
   - **Nice 权重映射**：内置紧凑的 40 项 `uint16_t sched_prio_to_weight[40]` 表（`nice 0` 对应权重 `1024`，每级相差约 1.25 倍）；
   - **虚拟运行时间推进**：每次执行 $\Delta t$ 微秒，$\Delta vruntime = \frac{\Delta t \times 1024}{\text{weight}}$，高权重（低 nice 值）任务的 `vruntime` 增长慢，获得更多 CPU 时间；
   - **睡眠补偿 (`place_entity`)**：新任务初始化为 `fair_rq->min_vruntime`；睡眠唤醒任务设下界 `min_vruntime - SCHED_LATENCY_US / 2`（默认延迟周期 `10,000 us`），防止长睡任务饿死其他任务；
   - **有序队列维护**：考虑到嵌入式常见并发就绪任务数（通常 < 32），`tasks_timeline` 采用侵入式 `run_node` 按 `vruntime` 升序插入，`pick_next_task` 直接 O(1) 取队首元素，零额外指针开销。
3. **`idle_sched_class` (`kernel/sched/sched_idle.c`)**：
   - 优先级最低，永远就绪保底；`pick_next_task` 直接返回 `rq->idle`。

### 4.3 抢占控制、延迟重调度与 SMP 负载均衡

1. **修复 `sch_lock` / `sch_unlock` 与异步切换**：
   - `sch_lock()` 递增 `scheduler_lock_nest`（及当前核 `rq->preempt_count`）；
   - 当 `scheduler_lock_nest > 0` 时调用 `switch_task()`，**不执行上下文切换**，而是设置 `this_rq()->need_resched = true` 并立即返回；
   - `sch_unlock()` 递减计数，当计数归零且 `this_rq()->need_resched` 为真（且不在中断上下文中）时，自动触发 `switch_task()`。
2. **SMP 选核与工作窃取 (`CONFIG_SMP=y`)**：
   - **唤醒选核 (`select_task_rq`)**：若 `p->cpu` 允许运行且当前为空闲则留在原核（保持缓存亲和性）；否则在 `p->cpus_allowed` 允许的核中选择负载最低（`nr_running` 最小 / `cpu_util_avg` 最低）的 CPU，并通过 `hal_smp_send_ipi(target_cpu, NOS_IPI_RESCHED)` 通知目标核。
   - **Idle / Tick 负载均衡 (`sched_balance_rq`)**：当某个 CPU 进入 Idle 前或周期性 Tick 触发时，扫描其他繁忙 CPU（`nr_running > 1`），将满足 `cpus_allowed` 的等待任务迁移（Pull）至本核执行。

---

## 5. 定点 PELT 负载跟踪与 Load Average 设计 (`kernel/sched/sched_load.c`)

1. **Per-Task 与 Per-CPU 定点 PELT (`0 .. 1024`)**：
   - 定义窗口长度 `SCHED_PELT_WINDOW_US = 100000U`（100ms）与满载容量 `SCHED_CAPACITY_SCALE = 1024U`；
   - 每次切换或 Tick 更新时，将增量 `delta_us` 累加到 `se.sum_exec_us` 与 `se.win_run_us`；
   - 若跨过窗口边界 `now_us - se.win_start_us >= SCHED_PELT_WINDOW_US`：
     - 计算上一窗口瞬时利用率 `win_util = min(1024, (win_run_us * 1024) / window_elapsed_us)`；
     - 按 EWMA 指数衰减公式更新：`util_avg = (util_avg * 3 + win_util) >> 2`；
     - 若任务睡眠跨越多于 1 个窗口（最多折叠衰减 8 个窗口），对空窗口连续执行 `util_avg = (util_avg * 3) >> 2`，实现平滑衰减；
   - **向后兼容 `get_cpu_usage()` 与 `task_get_cpu_usage()`**：
     - 现有 `app/tests/test.c` 通过 `task_get_cpu_usage(current) / 100` 获取百分比整数（期望返回 `0 .. 10000` 万分比标度，即 `100% == 10000`）；
     - 因此 `task_get_cpu_usage(task)` 与 `get_cpu_usage()` 将内部 `0..1024` 的实时负载精确换算为 `0..10000`（万分比：`util * 10000 / 1024`），既保证 `/ 100` 得到整数百分比，又可通过 `% 100` 得到两位小数百分比！
2. **系统级 1m / 5m / 15m 平均负载 (`avenrun`)**：
   - 采用 `FSHIFT = 11` (`FIXED_1 = 2048`) 定点格式维护 `g_avenrun[3]`；
   - 每 5 秒采样一次全系统活跃任务数 $n = \sum (\text{rq->nr\_running} + \text{rq->nr\_uninterruptible})$（不含 Idle），按 $a_t = a_{t-1} \cdot e^{-\Delta} + n \cdot 2048 \cdot (1 - e^{-\Delta})$ 更新；
   - 提供 `sched_get_loadavg(uint32_t loads[3], uint32_t *nr_running, uint32_t *nr_total)` 接口。

---

## 6. 小内存友好的跨任务栈回溯、异常 Dump 与增强型 `ps` 设计

### 6.1 专门针对 `wl-001` 等小内存 MCU 的零开销约束

`wl-001` (STM32F103) 仅有 **20KB SRAM**，Idle/Core 任务栈仅 **1024 字节**，`CONFIG_LOG_MESSAGE_SIZE=128`。因此栈回溯与 Dump 模块严格遵守以下铁律：
1. **零堆内存分配 (`0 kmalloc`)**：回溯与 Dump 全程不调用 `kmalloc`，保证在内存耗尽、关中断或 `HardFault` 异常上下文中 100% 安全可用。
2. **极小栈帧开销 (< 64 字节)**：禁止在回溯或 `dump_all_task` 函数栈上定义大数组（如 `char buf[256]`），每一帧直接通过 `print_fn` 流式格式化输出。
3. **零额外 ROM 符号大小推算**：不修改 `struct kallsym_entry` 结构体大小（保持 8 字节 `{addr, name}`），在 `kallsyms_lookup()` 中利用有序表相邻项 `kallsyms_table[i + 1].addr - kallsyms_table[i].addr` 动态计算 `symbolsize`，**0 字节 Flash 增长，0 字节 RAM 增长**。
4. **Kconfig 细粒度可裁剪**：提供 `CONFIG_KALLSYMS`（默认 `y`）与 `CONFIG_STACK_BACKTRACE`（默认 `y`），即使关闭 `kallsyms` 也能输出精简的 `[<0x0800xxxx>]` 原始 PC 调用链。

### 6.2 双模跨架构/跨任务栈回溯器 (`include/kernel/unwind.h` & `kernel/task/task_dump.c`)

1. **安全栈边界获取 (`task_get_stack_bounds`)**：
   - 根据 `task->stack` 与 `task->stack_size` 计算精确的 `[stack_low, stack_high)` 物理边界；
   - 若在中断/异常 Handler (`MSP`) 上下文回溯，边界自动切换为 `[&_estack - _Min_Stack_Size, &_estack)`，彻底消除现有 `0x20000000~0x30000000` 硬编码越界风险。
2. **跨任务上下文提取**：
   - **当前任务 (`task == current`)**：直接读取当前 CPU 的 `sp`、`lr`、`pc` 与 `fp`；
   - **非运行任务 (`task != current`)**：根据架构 `stack_init` / `PendSV` / `__arch_switch_to` 保存的栈帧结构（如 Cortex-M3/M4 在 `task->sp` 处保存的 `{[flag], r4-r11, r0-r3, r12, lr, pc, psr}`），精确提取被挂起/阻塞瞬间的 `pc`、`lr`、`r7/r11` 与异常帧后方的 `sp`；
   - **异常陷阱帧 (`HardFault` / `MemManage`)**：直接从 `struct cpu_dump_type` 提取故障瞬间的 `pc`、`lr`、`r7` 与故障前 `sp`。
3. **双模展开引擎（Frame Pointer + 指令感知栈扫描回退）**：
   - 先输出第 0 帧（当前/故障 `pc`）和 `lr`（若有效）；
   - **阶段 1（FP 链展开）**：若架构存在有效 Frame Pointer 链且 `fp` 严格递增且在 `[stack_low, stack_high)` 内，沿帧链提取返回地址；
   - **阶段 2（Stack Text Scanner 安全扫描回退）**：针对 Cortex-M `-O2` 默认省略帧指针的场景，从当前 `sp` 向 `stack_high` 按字对齐扫描（最多扫描 `min(stack_rem, 512)` 字节，最多输出 `CONFIG_BACKTRACE_MAX_DEPTH = 12` 层）：
     - 候选字 `val` 必须落在内核代码段 `[_stext, _etext)`（或命中 `kallsyms_table` 首尾区间）；
     - **ARM Thumb-2 指令校验**：`val` 的 bit 0 必须为 `1`（Thumb 状态返回地址），且 `pc = val & ~1UL` 的前一条指令 `*(uint16_t *)(pc - 4)` 必须是 32 位 `BL`/`BLX`（高 5 位 `0x1E` 且次字高 2 位 `0x3`）或 `*(uint16_t *)(pc - 2)` 是 16 位 `BLX Rm` (`(insn & 0xFF80) == 0x4780`)。这能以 0 表开销滤除 99% 以上的栈上伪地址与普通函数指针常量！

### 6.3 异常现场转储与增强型 `ps` / `procfs`

1. **增强型 `ps` 命令 (`app/shell/nshell.c` & `kernel/task/task_dump.c`)**：
   - `ps`（默认标准视图）：
     - 第一行输出系统负载汇总：`Tasks: N total, X run, Y sleep, Z stop | CPU: U.U% | Load: L1, L5, L15`
     - 表头与行：`PID PPID CPU POL PRIO STAT  %CPU     TIME  STK_USED/SIZE   CSW  WCHAN            NAME`
     - 若任务处于阻塞态 (`S` / `D`)，`WCHAN` 列优先显示 `wchan_name` 或通过 `kallsyms_lookup(task->wchan_addr)` 解析的阻塞函数名（如 `mutex_lock`、`sem_get`、`task_sleep`）。
   - `ps -l`（扩展视图）：额外显示 `BASE_PRIO`、`NICE`、`VRUNTIME`、`NVCSW`、`NIVCSW`、`AFFINITY` 及栈溢出诊断标志。
   - `ps -s <pid>` 或 `ps -w`（任务详情与跨任务栈回溯）：
     - `ps -s <pid>`：输出指定 PID 任务的完整调度实体信息、内存/栈边界、等待通道详情及其实时调用栈回溯 (`Call Trace`)；
     - `ps -w`：遍历所有任务并依次输出其调用栈回溯（类似 Linux `echo t > /proc/sysrq-trigger`）。
2. **Crash / Fault 自动完整转储**：
   - 在 `cpu_hard_fault()` / `MemManage_Handler()` / `BusFault_Handler()` / `UsageFault_Handler()` / `panic()` 中：
     - 打印故障类型、寄存器快照以及 `pc` / `lr` 对应的符号 (`[<addr>] sym+0xoff/0xsize`)；
     - 在 Cortex-M 上打印 `SCB->CFSR`、`HFSR`、`MMFAR`、`BFAR`；
     - 调用 `dump_trap_backtrace()` 打印故障点调用栈，随后打印栈顶 32 字节紧凑 Hex 摘要与全任务状态表 `dump_all_task()`。
3. **`procfs` 节点扩展 (`fs/procfs.c`)**：
   - `/proc/tasks`：升级为包含 `PID PPID CPU POL PRIO STAT %CPU STACK USED WCHAN NAME`；
   - `/proc/loadavg`：标准 Linux 格式 `0.05 0.02 0.00 1/6 <last_pid>`；
   - `/proc/stat`：导出 Per-CPU `busy_us` / `idle_us`、`ctxt`（总上下文切换次数）、`processes`、`procs_running`、`procs_blocked`。

---

## 7. 测试与验证计划

1. **Host 单元测试 (`tests/host/`)**：
   - `run_sched_multi_policy_test.py`：验证 Per-CPU `sched_rq`、`rt_sched_class` (`SCHED_FIFO` / `SCHED_RR` 位图与时间片轮转)、`fair_sched_class` (`SCHED_NORMAL` 虚拟运行时间 `vruntime` 与不同 `nice` 权重下的按比例 CPU 分配)、`idle_sched_class`、多锁优先级继承 (`task_pi_boost`/`unboost`) 以及 SMP 选核与 Work Stealing 负载均衡。
   - `run_sched_load_dump_test.py`：验证定点 PELT (`util_avg` 窗口累加与指数衰减)、系统 `loadavg` (1m/5m/15m)、`kallsyms` 二分查找与符号大小推算、跨任务栈回溯 (`dump_task_stack` FP 链与 Thumb-2 栈文本扫描回退) 以及 `ps` / `procfs` 格式化输出。
2. **现有回归与全板级编译验证**：
   - 运行 `python3 tests/run_host_regressions.py`、`python3 -m unittest discover -s tests -p 'test_*.py'`、`python3 scripts/check_layer_deps.py`；
   - 编译全部四个预设板级：`wl-001`（重点验证 20KB SRAM / 128KB Flash 零溢出）、`nk60-v2`、`qemu-stm32`、`zj-v3`；
   - 运行 `python3 tests/qemu_stm32_smoke.py build/qemu-stm32/nos.elf` 端到端 QEMU 冒烟验证。
