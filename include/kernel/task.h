/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_TASK_H__
#define __NOS_TASK_H__

#include <kernel/kernel.h>
#include <kernel/pid.h>
#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <kernel/timer.h>
#include <kernel/sched.h>
#include <asm/task.h>
#include <uapi/nos/ipc.h>
#include <nos/cap.h>
#include <nos/mm.h>

enum task_status {
    TASK_RUNNING   = 0x01,
    TASK_READY     = 0x02,
    TASK_SLEEPING  = 0x04,
    TASK_SUSPENDED = 0x08,
    TASK_ZOMBIE    = 0x10,
    TASK_BLOCKED   = 0x20,
};

/* Legacy status aliases for 100% source compatibility */
#define TASK_RUNING  TASK_RUNNING
#define TASK_WAIT    TASK_SLEEPING
#define TASK_SUSPEND TASK_SUSPENDED
#define TASK_CLOSE   TASK_ZOMBIE

#define TASK_PRIO_INVALID 0xFFU

struct sched_entity {
    struct list_head run_node;      /* Dedicated scheduler runqueue node */
    uint64_t         vruntime;      /* Virtual runtime in us (Fair class) */
    uint64_t         sum_exec_us;   /* Cumulative total execution time in us */
    uint64_t         exec_start_us; /* Timestamp when current slice started (us) */
    uint32_t         win_start_us;  /* Low 32-bit start time of current PELT window */
    uint32_t         win_run_us;    /* Active runtime in current PELT window (us) */
    uint16_t         util_avg;      /* Fixed-point PELT utilization (0..1024) */
    uint16_t         weight;        /* Fair scheduling weight (nice 0 = 1024) */
    uint32_t         nvcsw;         /* Voluntary context switches */
    uint32_t         nivcsw;        /* Involuntary (preempted) context switches */
    bool             on_rq;         /* Whether queued on a scheduler runqueue */
};

struct task_struct {
    addr_t *sp;

    const char *name;
    pid_t pid;
    pid_t ppid;

    addr_t *entry;
    addr_t *parameter;
    addr_t *stack;
    void *stack_alloc;

    uint32_t  init_tick;
    uint32_t  remaining_tick;

    /* Invoked by the reaper after the task's stack and PID are released. */
    void (*cleanup)(struct task_struct *task);

    uint8_t init_priority;
    uint8_t current_priority;
    uint8_t pi_boost_prio;
    uint8_t status;
    uint8_t policy;
    int8_t  nice;
    uint8_t cpu;
    uint8_t on_cpu;
    uint8_t cpus_allowed;
    int16_t exit_code;
    uint32_t stack_size;

#if CONFIG_MAX_PRIORITY > 32
    uint8_t  offset;
    uint8_t  prio_mask;
#endif
    uint32_t offset_mask;

    int flag;
    const struct sched_class *sched_class;
    struct sched_entity se;

    /* Wait-queue / close-list node (decoupled from se.run_node) */
    struct list_head list;
    struct list_head tlist;

    spinlock_t lock;
    spinlock_t *list_lock;
    struct timer timer;

    u64 start_time;
    u32 run_time;
    u32 sys_cycle;
    u32 save_run_time;
    u32 save_sys_cycle;

    /* Wait channel diagnostics (for ps / procfs / dump) */
    uintptr_t   wchan_addr;
    const char *wchan_name;

    /* Microkernel Capability Table & Synchronous/Async IPC State */
    struct nos_cap_table cap_table;
    struct nos_vm_space  vm_space;
    struct nos_ipc_msg   ipc_msg;
    struct task_struct  *ipc_partner;
    uint8_t              ipc_state;
    uint64_t             notify_bits;
};

enum task_ps_mode {
    TASK_PS_NORMAL = 0,
    TASK_PS_LONG   = 1,
    TASK_PS_STACK  = 2,
};

struct task_struct *task_current(void);
#define current task_current()

struct task_struct *task_create(const char *name,
                                void (*entry)(void *parameter),
                                void *parameter,
                                uint8_t priority,
                                uint32_t stack_size,
                                uint32_t tick,
                                void (*clean)(struct task_struct *task));
struct task_struct *task_create_ex(const char *name,
                                   void (*entry)(void *parameter),
                                   void *parameter,
                                   uint8_t policy,
                                   uint8_t priority,
                                   int8_t nice,
                                   uint8_t cpus_allowed,
                                   uint32_t stack_size,
                                   uint32_t tick,
                                   void (*clean)(struct task_struct *task));
int task_ready(struct task_struct *task);
int task_yield_cpu(void);
void task_del(struct task_struct *task);
void task_exit_with_code(int exit_code);
int task_waitpid(pid_t pid, int *status, uint32_t timeout_ticks);
int task_hang(struct task_struct *task);
int task_hang_lock(struct task_struct *task);
int task_suspend(struct task_struct *task);
int task_block_lock(struct task_struct *task, uint8_t wait_status,
                    const char *wchan_name, uintptr_t wchan_addr);
int task_resume(struct task_struct *task);
int task_sleep(u32 tick);
int task_set_prio(struct task_struct *task, uint8_t prio);
int task_pi_boost(struct task_struct *owner, uint8_t waiter_prio);
int task_pi_unboost(struct task_struct *owner, uint8_t next_waiter_prio);
int task_set_policy(struct task_struct *task, uint8_t policy, uint8_t prio, int8_t nice);
int task_set_nice(struct task_struct *task, int8_t nice);
int task_set_affinity(struct task_struct *task, uint8_t cpus_allowed);
void task_set_wchan(struct task_struct *task, const char *name, uintptr_t addr);
void task_clear_wchan(struct task_struct *task);
u32 task_get_cpu_usage(struct task_struct *task);
void clean_close_task(void);
void dump_all_task(void);
void dump_all_task_to(int (*print_fn)(const char *fmt, ...));
void dump_all_task_ex(int (*print_fn)(const char *fmt, ...), enum task_ps_mode mode);
int dump_task_detail(pid_t pid, int (*print_fn)(const char *fmt, ...), bool include_stack);
struct task_struct *task_find_by_pid(pid_t pid);
int task_foreach(int (*cb)(struct task_struct *task, void *arg), void *arg);
size_t task_stack_max_used(struct task_struct *task);
bool task_stack_is_healthy(struct task_struct *task);
void task_get_stack_bounds(struct task_struct *task, uintptr_t *low, uintptr_t *high);
const char *task_state_short_str(uint8_t status);
const char *task_policy_short_str(uint8_t policy);

#define task_list_lock(lock_func, lock) \
do {                                    \
    if ((lock) != NULL) {               \
        lock_func(lock);                \
    }                                   \
} while (0)

#define task_list_unlock(lock_func, lock) \
do {                                      \
    if ((lock) != NULL) {                 \
        lock_func(lock);                  \
    }                                     \
} while (0)

#endif /* __NOS_TASK_H__ */
