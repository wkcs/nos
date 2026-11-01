/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[TASK]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/task.h>
#include <kernel/sched.h>
#include <string.h>
#include <kernel/irq.h>
#include <kernel/cpu.h>
#include <kernel/mm.h>
#include <kernel/list.h>
#include <kernel/pid.h>
#include <kernel/spinlock.h>
#include <kernel/mutex.h>
#include <kernel/printk.h>
#include <kernel/timer.h>
#include <uapi/nos/abi.h>
#include <nos/ipc.h>
#include <nos/sched.h>
#include "stack_layout.h"

static LIST_HEAD(g_task_list);
LIST_HEAD(g_close_task_list);
static SPINLOCK(g_task_list_lock);
static SPINLOCK(g_close_list_lock);
static uint32_t g_nr_tasks;
static uint32_t g_task_foreach_active;

extern spinlock_t ready_list_lock;

uint32_t task_total_count(void)
{
    return g_nr_tasks;
}

static void task_clear_uninterruptible(struct task_struct *task)
{
    if (task != NULL && task->status == TASK_BLOCKED) {
        struct sched_rq *rq = sched_rq_of(task->cpu);
        if (rq != NULL && rq->nr_uninterruptible > 0U) {
            rq->nr_uninterruptible--;
        }
    }
}

static void task_update_prio_masks(struct task_struct *task, uint8_t prio)
{
    task->current_priority = prio;
#if CONFIG_MAX_PRIORITY > 32
    task->offset = prio >> 3;
    task->offset_mask = 1UL << task->offset;
    task->prio_mask = 1U << (prio & 0x07U);
#else
    task->offset_mask = 1UL << prio;
#endif
}

void task_set_wchan(struct task_struct *task, const char *name, uintptr_t addr)
{
    if (task == NULL) {
        return;
    }
    task->wchan_name = name;
    task->wchan_addr = addr;
}

void task_clear_wchan(struct task_struct *task)
{
    if (task == NULL) {
        return;
    }
    task->wchan_name = NULL;
    task->wchan_addr = 0U;
}

static void task_exit(void)
{
    struct task_struct *task;

    task = current;
    task_del(task);
}

void task_exit_with_code(int exit_code)
{
    struct task_struct *task = current;
    if (task != NULL) {
        task->exit_code = (int16_t)exit_code;
    }
    task_del(task);
}

static void timeout(void *parameter)
{
    struct task_struct *task;

    task = (struct task_struct *)parameter;

    BUG_ON(task == NULL);

    spin_lock_irq(&task->lock);
    if (task->status != TASK_SLEEPING && task->status != TASK_BLOCKED) {
        spin_unlock_irq(&task->lock);
        return;
    }
    task->flag = -ETIMEDOUT;
    if (task->list_lock) {
        spin_lock_irq(task->list_lock);
        list_del_init(&task->list);
        spin_unlock_irq(task->list_lock);
        task->list_lock = NULL;
    }
    task_clear_uninterruptible(task);
    task_clear_wchan(task);
    spin_unlock_irq(&task->lock);
    add_task_to_ready_list(task);
}

static void *alloc_task_stack(uint32_t stack_size, void **stack_base, uintptr_t *out_guard_base)
{
    char *buf;
    char *base;
    uint32_t i;

    if (out_guard_base != NULL) {
        *out_guard_base = 0U;
    }
#if defined(CONFIG_MPU_STACK_GUARD) && !defined(CONFIG_STACK_GROWSUP)
    if (stack_size > UINT32_MAX - (NOS_MPU_GUARD_SIZE + 31U)) {
        return NULL;
    }
    buf = kmalloc_aligned(stack_size + NOS_MPU_GUARD_SIZE, NOS_MPU_GUARD_SIZE, GFP_KERNEL);
    if (buf == NULL) {
        pr_err("alloc stack error\n");
        return NULL;
    }
    if (out_guard_base != NULL) {
        *out_guard_base = (uintptr_t)buf;
    }
    base = buf + NOS_MPU_GUARD_SIZE;
#else
    if (stack_size > UINT32_MAX - 15U) return NULL;
    buf = kmalloc(stack_size + 15U, GFP_KERNEL);
    if (buf == NULL) {
        pr_err("alloc stack error\n");
        return NULL;
    }

#ifdef CONFIG_STACK_GROWSUP
    base = (char *)task_stack_base((uintptr_t)buf, stack_size, true);
#else
    base = (char *)task_stack_base((uintptr_t)buf, stack_size, false);
#endif
#endif
    for (i = 0; i < stack_size; i++) base[i] = '#';
    *stack_base = base;

    return buf;
}

static int free_task_stack(void *stack_addr)
{
    return kfree(stack_addr);
}

static int __task_create(struct task_struct *task,
                         const char *name,
                         void (*entry)(void *parameter),
                         void *parameter,
                         addr_t *stack_start,
                         uint8_t policy,
                         uint8_t priority,
                         int8_t nice,
                         uint8_t cpus_allowed,
                         uint32_t tick,
                         void (*clean)(struct task_struct *task))
{
    int rc;
    struct task_struct *parent = current;

    INIT_LIST_HEAD(&task->list);
    INIT_LIST_HEAD(&task->tlist);
    INIT_LIST_HEAD(&task->held_mutex_list);
    INIT_LIST_HEAD(&task->se.run_node);
    spin_lock_init(&task->lock);

    task->list_lock = NULL;
    task->name = name;
    task->ppid = (parent != NULL) ? parent->pid : 0U;
    task->entry = (void *)entry;
    task->parameter = parameter;
    task->stack = stack_start;
    task->sp = (addr_t *)stack_init(task->entry, task->parameter,
                                    (addr_t *)task->stack,
                                    (void *)task_exit);
    task->init_priority    = priority;
    task->pi_boost_prio    = TASK_PRIO_INVALID;
    task_update_prio_masks(task, priority);

    task->policy       = policy;
    task->nice         = nice;
    task->cpu          = 0U;
    task->on_cpu       = 0U;
    task->cpus_allowed = (cpus_allowed != 0U) ? cpus_allowed : NOS_CPU_MASK_ALL;
    task->exit_code    = 0;
    task->sched_class  = sched_policy_to_class(policy);
    sched_load_task_init(task, nice);

    task->init_tick      = tick;
    task->remaining_tick = tick;
    task->status  = TASK_SUSPENDED;
    task->cleanup = clean;
    task->flag = 0;
    task->start_time = 0;
    task->wchan_addr = 0U;
    task->wchan_name = NULL;
    task->ipc_partner = NULL;
    task->ipc_state = NOS_IPC_STATE_NONE;
    task->notify_bits = 0;
    memset(&task->ipc_msg, 0, sizeof(task->ipc_msg));
    nos_cap_table_init(&task->cap_table);
    (void)nos_cap_alloc_at(&task->cap_table,
                           NOS_CAP_SLOT_SELF_TASK,
                           NOS_CAP_TASK,
                           task,
                           NOS_CAP_RIGHT_ALL,
                           (uint32_t)task->pid,
                           NULL);
    (void)nos_cap_alloc_at(&task->cap_table,
                           NOS_CAP_SLOT_SELF_VMS,
                           NOS_CAP_VM_SPACE,
                           &task->vm_space,
                           NOS_CAP_RIGHT_ALL,
                           (uint32_t)task->pid,
                           NULL);

    spin_lock_irq(&g_task_list_lock);
    list_add_tail(&task->tlist, &g_task_list);
    g_nr_tasks++;
    spin_unlock_irq(&g_task_list_lock);
    rc = timer_init(&task->timer, task->name, timeout, task);
    if (rc < 0) {
        pr_err("%s init timer error, rc=%d\n", task->name, rc);
    }

    return rc;
}

struct task_struct *task_create_ex(const char *name,
                                   void (*entry)(void *parameter),
                                   void *parameter,
                                   uint8_t policy,
                                   uint8_t priority,
                                   int8_t nice,
                                   uint8_t cpus_allowed,
                                   uint32_t stack_size,
                                   uint32_t tick,
                                   void (*clean)(struct task_struct *task))
{
    struct task_struct *task;
    addr_t *stack_start;
    pid_t pid;
    void *stack_addr;
    void *stack_base;
    uintptr_t guard_base = 0U;
    int rc;

#if CONFIG_MAX_PRIORITY < 256
    if (priority >= CONFIG_MAX_PRIORITY) {
        pr_err("%s: Priority should be less than %d\n", name, CONFIG_MAX_PRIORITY);
        return NULL;
    }
#endif

    task = kmalloc(sizeof(struct task_struct), GFP_KERNEL);
    if (task == NULL) {
        pr_err("%s: alloc task struct buf error\n", name);
        goto task_struct_err;
    }

    task->stack_size = stack_size;
    stack_addr = alloc_task_stack(stack_size, &stack_base, &guard_base);
    if (stack_addr == NULL) {
        pr_err("%s: alloc task stack buf error\n", name);
        goto task_stack_err;
    }
    (void)hal_vm_space_init(&task->vm_space);
    if (guard_base != 0U) {
        (void)nos_mpu_set_stack_guard(&task->vm_space, guard_base);
    }
#ifdef CONFIG_STACK_GROWSUP
    stack_start = (addr_t *)(stack_base);
#else
    stack_start = (addr_t *)((addr_t)stack_base + stack_size - sizeof(addr_t));
#endif
    task->stack_alloc = stack_addr;

    pid = pid_alloc();
    if (!pid) {
        pr_err("%s: alloc pid error\n", name);
        goto pid_err;
    }

    task->pid = pid;

    rc = __task_create(task, name, entry, parameter, stack_start,
                       policy, priority, nice, cpus_allowed, tick, clean);
    if (rc < 0) {
        pr_err("%s: task_create error, rc=%d\n", name, rc);
        goto task_create_err;
    }

    return task;

task_create_err:
    pid_free(pid);
pid_err:
    free_task_stack(stack_addr);
task_stack_err:
    kfree(task);
task_struct_err:
    return NULL;
}

struct task_struct *task_create(const char *name,
                                void (*entry)(void *parameter),
                                void *parameter,
                                uint8_t priority,
                                uint32_t stack_size,
                                uint32_t tick,
                                void (*clean)(struct task_struct *task))
{
    uint8_t policy = SCHED_POLICY_RR;

    if (priority == IDEL_TASK_PRIO && name != NULL &&
        (strcmp(name, "idel") == 0 || strncmp(name, "idle", 4) == 0)) {
        policy = SCHED_POLICY_IDLE;
    }
    return task_create_ex(name, entry, parameter, policy, priority, 0,
                          NOS_CPU_MASK_ALL, stack_size, tick, clean);
}

int task_ready(struct task_struct *task)
{
    if (!task) {
        pr_err("task struct is NULL\n");
        return -EINVAL;
    }

    if (task->status == TASK_READY || task->status == TASK_RUNNING) {
        pr_warning("%s:task is already in a ready state\n", task->name);
        return -EINVAL;
    }

    add_task_to_ready_list(task);
    switch_task();

    return 0;
}

int task_yield_cpu(void)
{
    struct task_struct *task;

    task = current;
    if (task == NULL) {
        return -EINVAL;
    }
    spin_lock_irq(&task->lock);
    if (task->status == TASK_RUNNING) {
        del_task_to_ready_list_lock(task);
        add_task_to_ready_list_lock(task);
    }
    spin_unlock_irq(&task->lock);
    switch_task();

    return 0;
}

void task_del(struct task_struct *task)
{
    spinlock_t *list_lock;

    if (!task) {
        pr_err("task struct is NULL\n");
        return;
    }

    pr_info("del %s task\n", task->name);
    spin_lock_irq(&task->lock);
    list_lock = task->list_lock;
    task_list_lock(spin_lock_irq, list_lock);
    if (!list_empty(&task->list)) {
        list_del_init(&task->list);
    }
    task_list_unlock(spin_unlock_irq, list_lock);
    task->list_lock = NULL;

    task_clear_uninterruptible(task);
    if (task->status == TASK_READY || task->status == TASK_RUNNING || task->se.on_rq) {
        del_task_to_ready_list_lock(task);
    }

    spin_lock_irq(&g_task_list_lock);
    list_del(&task->tlist);
    if (g_nr_tasks > 0U) {
        g_nr_tasks--;
    }
    spin_unlock_irq(&g_task_list_lock);
    task->status = TASK_ZOMBIE;
    timer_stop(&task->timer);
    task_clear_wchan(task);
    task->list_lock = &g_close_list_lock;
    spin_lock_irq(task->list_lock);
    list_add_tail(&task->list, &g_close_task_list);
    spin_unlock_irq(task->list_lock);
    spin_unlock_irq(&task->lock);

    if (task == current) {
        switch_task();
    }
}

int task_block_lock(struct task_struct *task, uint8_t wait_status,
                    const char *wchan_name, uintptr_t wchan_addr)
{
    if (!task) {
        pr_err("task struct is NULL\n");
        return -EINVAL;
    }
    if (task->status != TASK_READY && task->status != TASK_RUNNING) {
        pr_err("%s task status is %d, not is TASK_READY or TASK_RUNNING\n",
               task->name, task->status);
        BUG_ON(task == current);
        return -EINVAL;
    }

    timer_stop(&task->timer);
    del_task_to_ready_list_lock(task);
    task->status = wait_status;
    if (wait_status == TASK_BLOCKED) {
        struct sched_rq *rq = sched_rq_of(task->cpu);
        if (rq != NULL) {
            rq->nr_uninterruptible++;
        }
    }
    task_set_wchan(task, wchan_name, wchan_addr);

    return 0;
}

int task_hang(struct task_struct *task)
{
    int rc;

    if (!task) {
        pr_err("task struct is NULL\n");
        return -EINVAL;
    }
    spin_lock_irq(&task->lock);
    rc = task_block_lock(task, TASK_SUSPENDED, "suspended",
                         (uintptr_t)__builtin_return_address(0));
    spin_unlock_irq(&task->lock);
    if (rc == 0 && task == current) {
        switch_task();
    }
    return rc;
}

int task_suspend(struct task_struct *task)
{
    int rc;

    if (!task) {
        return -EINVAL;
    }
    spin_lock_irq(&task->lock);
    rc = task_block_lock(task, TASK_SUSPENDED, "suspended",
                         (uintptr_t)__builtin_return_address(0));
    spin_unlock_irq(&task->lock);
    if (rc == 0 && task == current) {
        switch_task();
    }
    return rc;
}

int task_hang_lock(struct task_struct *task)
{
    uint8_t wait_status = TASK_SLEEPING;

    if (task != NULL && task->wchan_name != NULL &&
        (strcmp(task->wchan_name, "mutex") == 0 ||
         strcmp(task->wchan_name, "ipc_reply") == 0 ||
         strcmp(task->wchan_name, "ipc_send") == 0)) {
        wait_status = TASK_BLOCKED;
    }
    return task_block_lock(task, wait_status,
                           task && task->wchan_name ? task->wchan_name : "wait",
                           task && task->wchan_addr ? task->wchan_addr :
                           (uintptr_t)__builtin_return_address(0));
}

int task_resume(struct task_struct *task)
{
    if (!task) {
        pr_err("task struct is NULL\n");
        return -EINVAL;
    }
    if (task->status != TASK_SLEEPING &&
        task->status != TASK_BLOCKED &&
        task->status != TASK_SUSPENDED) {
        pr_err("task(=%s) status(=%d) is not sleeping/blocked/suspended\n",
               task->name, task->status);
        BUG_ON(true);
        return -EINVAL;
    }

    timer_stop(&task->timer);
    task_clear_uninterruptible(task);
    task_clear_wchan(task);
    add_task_to_ready_list(task);

    return 0;
}

int task_waitpid(pid_t pid, int *status, uint32_t timeout_ticks)
{
    uint32_t waited = 0U;

    if (pid == 0U) {
        return -EINVAL;
    }
    while (task_find_by_pid(pid) != NULL) {
        if (timeout_ticks > 0U && waited >= timeout_ticks) {
            return -ETIMEDOUT;
        }
        (void)task_sleep(1U);
        waited++;
    }
    if (status != NULL) {
        *status = 0;
    }
    return 0;
}

int task_sleep(u32 tick)
{
    struct task_struct *task;
    int rc;

    if (!tick) {
        pr_err("sleep tick is 0\n");
        return -EINVAL;
    }

    task = current;
    spin_lock_irq(&task->lock);
    rc = task_block_lock(task, TASK_SLEEPING, "sleep",
                         (uintptr_t)__builtin_return_address(0));
    if (rc) {
        spin_unlock_irq(&task->lock);
        pr_err("%s task hang error, rc=%d\n", task->name, rc);
        return rc;
    }
    timer_start(&task->timer, tick);
    spin_unlock_irq(&task->lock);
    switch_task();

    if (task->flag == -ETIMEDOUT)
        task->flag = 0;

    return 0;
}

static void task_apply_prio_locked(struct task_struct *task, uint8_t eff_prio)
{
    if (task->current_priority == eff_prio) {
        return;
    }

    if (task->status == TASK_READY || task->status == TASK_RUNNING || task->se.on_rq) {
        del_task_to_ready_list_lock(task);
        task_update_prio_masks(task, eff_prio);
        add_task_to_ready_list_lock(task);
    } else {
        task_update_prio_masks(task, eff_prio);
    }
}

int task_set_prio(struct task_struct *task, uint8_t prio)
{
    uint8_t eff_prio;

    if (task == NULL) {
        pr_err("task is NULL\n");
        return -EINVAL;
    }

    spin_lock_irq(&task->lock);
    task->init_priority = prio;
    eff_prio = task->init_priority;
    if (task->pi_boost_prio != TASK_PRIO_INVALID && task->pi_boost_prio < eff_prio) {
        eff_prio = task->pi_boost_prio;
    }
    task_apply_prio_locked(task, eff_prio);
    spin_unlock_irq(&task->lock);

    return 0;
}

int task_pi_boost(struct task_struct *owner, uint8_t waiter_prio)
{
    uint8_t eff_prio;

    if (owner == NULL) {
        return -EINVAL;
    }
    spin_lock_irq(&owner->lock);
    if (waiter_prio < owner->pi_boost_prio) {
        owner->pi_boost_prio = waiter_prio;
    }
    eff_prio = (owner->pi_boost_prio < owner->init_priority) ?
               owner->pi_boost_prio : owner->init_priority;
    task_apply_prio_locked(owner, eff_prio);
    spin_unlock_irq(&owner->lock);
    return 0;
}

int task_pi_unboost(struct task_struct *owner, uint8_t next_waiter_prio)
{
    uint8_t best_boost;
    uint8_t eff_prio;

    if (owner == NULL) {
        return -EINVAL;
    }
    spin_lock_irq(&owner->lock);
    best_boost = next_waiter_prio;
    if (owner->held_mutex_list.next != NULL) {
        struct mutex *m;
        list_for_each_entry(m, &owner->held_mutex_list, held_node) {
            if (!list_empty(&m->wait_list)) {
                struct task_struct *w =
                    list_first_entry(&m->wait_list, struct task_struct, list);
                if (w->current_priority < best_boost) {
                    best_boost = w->current_priority;
                }
            }
        }
    }
    if (owner->ipc_partner != NULL &&
        owner->ipc_partner->ipc_state == NOS_IPC_STATE_REPLY_WAIT &&
        owner->ipc_partner->current_priority < best_boost) {
        best_boost = owner->ipc_partner->current_priority;
    }
    owner->pi_boost_prio = best_boost;
    eff_prio = (owner->pi_boost_prio < owner->init_priority) ?
               owner->pi_boost_prio : owner->init_priority;
    task_apply_prio_locked(owner, eff_prio);
    spin_unlock_irq(&owner->lock);
    return 0;
}

int task_set_policy(struct task_struct *task, uint8_t policy, uint8_t prio, int8_t nice)
{
    bool was_on_rq;

    if (task == NULL || policy > SCHED_POLICY_IDLE) {
        return -EINVAL;
    }

    spin_lock_irq(&task->lock);
    was_on_rq = (task->status == TASK_READY || task->status == TASK_RUNNING || task->se.on_rq);
    if (was_on_rq) {
        del_task_to_ready_list_lock(task);
    }
    task->policy = policy;
    task->init_priority = prio;
    task->nice = nice;
    task->se.weight = sched_nice_to_weight(nice);
    task->sched_class = sched_policy_to_class(policy);
    task_update_prio_masks(task, prio);
    if (was_on_rq) {
        add_task_to_ready_list_lock(task);
    }
    spin_unlock_irq(&task->lock);

    if (was_on_rq) {
        switch_task();
    }
    return 0;
}

int task_set_nice(struct task_struct *task, int8_t nice)
{
    if (task == NULL) {
        return -EINVAL;
    }
    if (nice < -20) {
        nice = -20;
    } else if (nice > 19) {
        nice = 19;
    }
    spin_lock_irq(&task->lock);
    task->nice = nice;
    task->se.weight = sched_nice_to_weight(nice);
    spin_unlock_irq(&task->lock);
    return 0;
}

int task_set_affinity(struct task_struct *task, uint8_t cpus_allowed)
{
    if (task == NULL || cpus_allowed == 0U) {
        return -EINVAL;
    }
    spin_lock_irq(&task->lock);
    task->cpus_allowed = cpus_allowed;
    spin_unlock_irq(&task->lock);
    return 0;
}

u32 task_get_cpu_usage(struct task_struct *task)
{
    uint16_t util;

    if (task == NULL) {
        pr_err("task is NULL\n");
        return 0;
    }

    util = sched_load_task_util(task, cpu_run_time_us());
    return ((u32)util * 10000U) / SCHED_CAPACITY_SCALE;
}

void task_get_stack_bounds(struct task_struct *task, uintptr_t *low, uintptr_t *high)
{
    if (task == NULL || task->stack == NULL || task->stack_size == 0U) {
        if (low != NULL) *low = 0U;
        if (high != NULL) *high = 0U;
        return;
    }
#ifndef CONFIG_STACK_GROWSUP
    if (low != NULL) {
        *low = (uintptr_t)task->stack - task->stack_size + sizeof(addr_t);
    }
    if (high != NULL) {
        *high = (uintptr_t)task->stack + sizeof(addr_t);
    }
#else
    if (low != NULL) {
        *low = (uintptr_t)task->stack;
    }
    if (high != NULL) {
        *high = (uintptr_t)task->stack + task->stack_size;
    }
#endif
}

bool task_stack_is_healthy(struct task_struct *task)
{
    if (task == NULL || task->stack == NULL || task->stack_size == 0U) {
        return false;
    }
#ifndef CONFIG_STACK_GROWSUP
    if (*(char *)((addr_t)task->stack - task->stack_size + sizeof(addr_t)) != '#')
        return false;
    else
        return true;
#else
    if (*(char *)((addr_t)task->stack + task->stack_size) != '#')
        return false;
    else
        return true;
#endif
}

static size_t task_get_stack_max_used(struct task_struct *task)
{
    size_t used, i;
    addr_t addr;

    if (!task_stack_is_healthy(task))
        return 0;

    used = task->stack_size;

#ifndef CONFIG_STACK_GROWSUP
    addr = (addr_t)task->stack - task->stack_size + sizeof(addr_t);
    for (i = 0; i < task->stack_size; i++) {
        if (*(char *)(addr + i) == '#')
            used--;
        else
            return used;
    }
#elif
    addr = (addr_t)task->stack + task->stack_size;
    for (i = 0; i < task->stack_size; i++) {
        if (*(char *)(addr - i) == '#')
            used--;
        else
            return used;
    }
#endif

    return used;
}

size_t task_stack_max_used(struct task_struct *task)
{
    if (task == NULL) {
        return 0;
    }
    return task_get_stack_max_used(task);
}

struct task_struct *task_find_by_pid(pid_t pid)
{
    struct task_struct *task_temp;

    spin_lock_irq(&g_task_list_lock);
    list_for_each_entry(task_temp, &g_task_list, tlist) {
        if (task_temp->pid == pid) {
            spin_unlock_irq(&g_task_list_lock);
            return task_temp;
        }
    }
    spin_unlock_irq(&g_task_list_lock);
    return NULL;
}

int task_foreach(int (*cb)(struct task_struct *task, void *arg), void *arg)
{
    struct task_struct *task_temp, *tmp;
    uint32_t idx = 0U;
    int rc = 0;

    if (cb == NULL) {
        return -EINVAL;
    }
#if CONFIG_NR_CPUS <= 1
    if (g_task_list_lock.rlock.raw_lock.tickets.owner !=
        g_task_list_lock.rlock.raw_lock.tickets.next) {
        list_for_each_entry_safe(task_temp, tmp, &g_task_list, tlist) {
            rc = cb(task_temp, arg);
            if (rc != 0) {
                break;
            }
        }
        return rc;
    }
#endif
    spin_lock_irq(&g_task_list_lock);
    g_task_foreach_active++;
    spin_unlock_irq(&g_task_list_lock);

    for (idx = 0U; ; idx++) {
        uint32_t cur = 0U;
        struct task_struct *picked = NULL;

        spin_lock_irq(&g_task_list_lock);
        list_for_each_entry(task_temp, &g_task_list, tlist) {
            if (cur == idx) {
                picked = task_temp;
                break;
            }
            cur++;
        }
        spin_unlock_irq(&g_task_list_lock);

        if (picked == NULL) {
            break;
        }
        rc = cb(picked, arg);
        if (rc != 0) {
            break;
        }
    }

    spin_lock_irq(&g_task_list_lock);
    if (g_task_foreach_active > 0U) {
        g_task_foreach_active--;
    }
    spin_unlock_irq(&g_task_list_lock);
    return rc;
}

const char *task_state_short_str(uint8_t status)
{
    switch (status) {
    case TASK_RUNNING:   return "R";
    case TASK_READY:     return "R";
    case TASK_SLEEPING:  return "S";
    case TASK_BLOCKED:   return "D";
    case TASK_SUSPENDED: return "T";
    case TASK_ZOMBIE:    return "Z";
    default:             return "?";
    }
}

const char *task_policy_short_str(uint8_t policy)
{
    switch (policy) {
    case SCHED_POLICY_RR:     return "RR";
    case SCHED_POLICY_FIFO:   return "FF";
    case SCHED_POLICY_NORMAL: return "TS";
    case SCHED_POLICY_IDLE:   return "IDL";
    default:                  return "?";
    }
}

static void task_free(struct task_struct *task)
{
    hal_vm_space_destroy(&task->vm_space);
    nos_cap_table_destroy(&task->cap_table);
    pid_free(task->pid);
    free_task_stack(task->stack_alloc);
    task->stack_alloc = NULL;
    if (task->cleanup != NULL) task->cleanup(task);
    kfree(task);
}

void clean_close_task(void)
{
    struct task_struct *task, *tmp;
    LIST_HEAD(tmp_list);

    if (READ_ONCE(g_task_foreach_active) != 0U) {
        return;
    }

    spin_lock_irq(&g_close_list_lock);
    list_for_each_entry_safe(task, tmp, &g_close_task_list, list) {
        if (task->status != TASK_CLOSE) {
            pr_fatal("%s task status is not close\n", task->name);
            list_del(&task->list);
        } else {
            list_del(&task->list);
            list_add_tail(&task->list, &tmp_list);
        }
    }
    spin_unlock_irq(&g_close_list_lock);

    list_for_each_entry_safe (task, tmp, &tmp_list, list) {
        pr_info("free %s task\n", task->name);
        task_free(task);
    }
}
