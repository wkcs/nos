/**
 * Copyright (C) 2023-2026 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[SCH]:%s[%d]:"fmt, __func__, __LINE__

#include <nos/sched.h>
#include <kernel/sched.h>
#include <kernel/kernel.h>
#include <kernel/task.h>
#include <kernel/list.h>
#include <kernel/cpu.h>
#include <kernel/spinlock.h>
#include <kernel/init.h>
#include <kernel/printk.h>
#include <nos/hal_arch.h>
#include <nos/mm.h>
#include <string.h>

struct sched_rq g_sched_rqs[CONFIG_NR_CPUS];
SPINLOCK(ready_list_lock);
uint32_t scheduler_lock_nest;

u32 g_sys_cycle;
u64 sys_heartbeat_time;

struct sched_rq *sched_rq_of(uint32_t cpu_id)
{
    if (cpu_id >= CONFIG_NR_CPUS) {
        return &g_sched_rqs[0];
    }
    return &g_sched_rqs[cpu_id];
}

struct sched_rq *this_rq(void)
{
#if CONFIG_NR_CPUS > 1
    uint32_t cpu = hal_cpu_id();

    if (cpu >= CONFIG_NR_CPUS) {
        cpu = 0U;
    }
    return &g_sched_rqs[cpu];
#else
    return &g_sched_rqs[0];
#endif
}

struct task_struct *task_current(void)
{
    return this_rq()->curr;
}

const struct sched_class *sched_policy_to_class(uint8_t policy)
{
    switch (policy) {
    case SCHED_POLICY_IDLE:
        return &idle_sched_class;
#ifdef CONFIG_SCHED_FAIR
    case SCHED_POLICY_NORMAL:
        return &fair_sched_class;
#endif
    case SCHED_POLICY_FIFO:
    case SCHED_POLICY_RR:
    default:
        return &rt_sched_class;
    }
}

static struct task_struct *pick_next_task_all(struct sched_rq *rq)
{
    const struct sched_class *class = &rt_sched_class;
    struct task_struct *p;

    while (class != NULL) {
        p = class->pick_next_task(rq);
        if (p != NULL) {
            return p;
        }
        class = class->next;
    }
    return (rq != NULL) ? rq->idle : NULL;
}

static bool is_legacy_idle_task(const struct task_struct *p)
{
    if (p == NULL) {
        return false;
    }
    if (p->policy == SCHED_POLICY_IDLE) {
        return true;
    }
    if (p->current_priority == IDEL_TASK_PRIO &&
        p->name != NULL &&
        (strcmp(p->name, "idel") == 0 || strcmp(p->name, "idle") == 0)) {
        return true;
    }
    return false;
}

void sched_enqueue_task_locked(struct sched_rq *rq, struct task_struct *p, int flags)
{
    if (rq == NULL || p == NULL) {
        return;
    }

    if (is_legacy_idle_task(p)) {
        p->policy = SCHED_POLICY_IDLE;
        p->sched_class = &idle_sched_class;
        if (rq->idle == NULL) {
            rq->idle = p;
        }
    }

    p->sched_class = sched_policy_to_class(p->policy);
    if (p->se.run_node.next == NULL || p->se.run_node.prev == NULL) {
        INIT_LIST_HEAD(&p->se.run_node);
    }
    if (p->cpus_allowed == 0U) {
        p->cpus_allowed = NOS_CPU_MASK_ALL;
    }
    p->cpu = (uint8_t)rq->cpu_id;

    if (p->sched_class->enqueue_task != NULL) {
        p->sched_class->enqueue_task(rq, p, flags);
    }

    if (p == rq->curr) {
        p->status = TASK_RUNNING;
    } else {
        p->status = TASK_READY;
    }

    if (p->sched_class->check_preempt_curr != NULL && p != rq->curr) {
        p->sched_class->check_preempt_curr(rq, p, flags);
    }
}

void sched_dequeue_task_locked(struct sched_rq *rq, struct task_struct *p, int flags)
{
    if (rq == NULL || p == NULL) {
        return;
    }

    if (p->sched_class == NULL) {
        p->sched_class = sched_policy_to_class(p->policy);
    }
    if (p->se.on_rq && p->sched_class->dequeue_task != NULL) {
        p->sched_class->dequeue_task(rq, p, flags);
    }
}

uint32_t sched_select_task_rq(struct task_struct *p, int wake_flags)
{
    (void)wake_flags;
#if CONFIG_NR_CPUS <= 1
    (void)p;
    return 0U;
#else
    uint8_t mask;
    uint32_t prev_cpu;
    uint32_t best_cpu = 0U;
    uint32_t best_nr = U32_MAX;
    uint16_t best_util = U16_MAX;
    bool found = false;
    uint32_t cpu;

    if (p == NULL) {
        return 0U;
    }

    prev_cpu = (p->cpu < CONFIG_NR_CPUS) ? p->cpu : 0U;
    if (p->on_cpu != 0U) {
        return prev_cpu;
    }

    mask = (p->cpus_allowed != 0U) ? p->cpus_allowed : NOS_CPU_MASK_ALL;
    if (g_sched_rqs[prev_cpu].online &&
        (mask & (1U << prev_cpu)) != 0U &&
        g_sched_rqs[prev_cpu].nr_running == 0U) {
        return prev_cpu;
    }

    for (cpu = 0U; cpu < CONFIG_NR_CPUS; cpu++) {
        struct sched_rq *rq = &g_sched_rqs[cpu];

        if (!rq->online || (mask & (1U << cpu)) == 0U) {
            continue;
        }
        if (!found ||
            rq->nr_running < best_nr ||
            (rq->nr_running == best_nr && rq->cpu_util_avg < best_util)) {
            best_cpu = cpu;
            best_nr = rq->nr_running;
            best_util = rq->cpu_util_avg;
            found = true;
        }
    }

    return found ? best_cpu : prev_cpu;
#endif
}

#if CONFIG_NR_CPUS > 1
static int sched_balance_rq_locked(struct sched_rq *dst_rq)
{
    struct sched_rq *busiest = NULL;
    struct task_struct *pos;
    struct task_struct *victim = NULL;
    uint32_t max_nr = 0U;
    uint32_t cpu;
    uint32_t prio;
    uint8_t dst_bit;

    if (dst_rq == NULL || !dst_rq->online) {
        return 0;
    }

    dst_bit = (uint8_t)(1U << dst_rq->cpu_id);
    for (cpu = 0U; cpu < CONFIG_NR_CPUS; cpu++) {
        struct sched_rq *src = &g_sched_rqs[cpu];

        if (src == dst_rq || !src->online) {
            continue;
        }
        if (src->nr_running > 1U &&
            src->nr_running > dst_rq->nr_running + 1U &&
            src->nr_running > max_nr) {
            max_nr = src->nr_running;
            busiest = src;
        }
    }

    if (busiest == NULL) {
        return 0;
    }

    if (busiest->cpu_id < dst_rq->cpu_id) {
        raw_spin_unlock(&dst_rq->lock.rlock);
        raw_spin_lock(&busiest->lock.rlock);
        raw_spin_lock(&dst_rq->lock.rlock);
    } else {
        raw_spin_lock(&busiest->lock.rlock);
    }

    list_for_each_entry(pos, &busiest->fair.tasks_timeline, se.run_node) {
        uint8_t mask = (pos->cpus_allowed != 0U) ? pos->cpus_allowed : NOS_CPU_MASK_ALL;

        if (pos->on_cpu == 0U && pos != busiest->curr && (mask & dst_bit) != 0U) {
            victim = pos;
            break;
        }
    }

    if (victim == NULL) {
        for (prio = 0U; prio < CONFIG_MAX_PRIORITY && victim == NULL; prio++) {
            list_for_each_entry(pos, &busiest->rt.queue[prio], se.run_node) {
                uint8_t mask = (pos->cpus_allowed != 0U) ?
                               pos->cpus_allowed : NOS_CPU_MASK_ALL;

                if (pos->on_cpu == 0U && pos != busiest->curr &&
                    (mask & dst_bit) != 0U) {
                    victim = pos;
                    break;
                }
            }
        }
    }

    if (victim == NULL) {
        raw_spin_unlock(&busiest->lock.rlock);
        return 0;
    }

    sched_dequeue_task_locked(busiest, victim, SCHED_DEQUEUE_MIGRATE);
    victim->cpu = (uint8_t)dst_rq->cpu_id;
    sched_enqueue_task_locked(dst_rq, victim, SCHED_ENQUEUE_MIGRATE);
    raw_spin_unlock(&busiest->lock.rlock);
    return 1;
}
#endif

int sched_balance_rq(uint32_t this_cpu)
{
#if CONFIG_NR_CPUS <= 1
    (void)this_cpu;
    return 0;
#else
    struct sched_rq *dst_rq;
    int moved;

    if (this_cpu >= CONFIG_NR_CPUS) {
        return 0;
    }
    dst_rq = &g_sched_rqs[this_cpu];
    spin_lock_irq(&dst_rq->lock);
    moved = sched_balance_rq_locked(dst_rq);
    spin_unlock_irq(&dst_rq->lock);
    return moved;
#endif
}

void sch_init(void)
{
    uint32_t i;

    scheduler_lock_nest = 0U;
    g_sys_cycle = 0U;
    sys_heartbeat_time = 0ULL;
    spin_lock_init(&ready_list_lock);
    sched_load_init();

    for (i = 0U; i < CONFIG_NR_CPUS; i++) {
        struct sched_rq *rq = &g_sched_rqs[i];

        memset(rq, 0, sizeof(*rq));
        spin_lock_init(&rq->lock);
        rq->cpu_id = i;
        rq->online = true;
        sched_rt_rq_init(&rq->rt);
        sched_fair_rq_init(&rq->fair);
    }

    idel_task_init();
}

void sch_start(void)
{
    struct sched_rq *rq = this_rq();
    struct task_struct *to_task;
    uint64_t now_us;

    to_task = pick_next_task_all(rq);
    BUG_ON(to_task == NULL);

    g_sys_cycle = 0U;
    now_us = cpu_run_time_us();
    sys_heartbeat_time = now_us;

    rq->clock_us = now_us;
    rq->win_start_us = (uint32_t)now_us;
    rq->curr = to_task;
    rq->need_resched = false;

    to_task->status = TASK_RUNNING;
    to_task->cpu = (uint8_t)rq->cpu_id;
    to_task->on_cpu = 1U;
    to_task->start_time = now_us;
    to_task->se.exec_start_us = now_us;
    if (to_task->se.win_start_us == 0U) {
        to_task->se.win_start_us = (uint32_t)now_us;
    }
#ifdef CONFIG_MPU
    arch_mpu_init();
#endif
    hal_vm_switch(&to_task->vm_space);
    context_switch_to((addr_t)&to_task->sp);

    /* never come back */
}

static void calculate_sys_cycle(void)
{
    static int count = 0;
#ifdef CONFIG_SYS_TICK_MS
    const int ticks_per_sec = (CONFIG_SYS_TICK_MS > 0) ? (1000 / CONFIG_SYS_TICK_MS) : 1000;
#else
    const int ticks_per_sec = 1000;
#endif

    count++;
    if (count >= ticks_per_sec) {
        count = 0;
        g_sys_cycle++;
    }
}

void switch_task(void)
{
    struct sched_rq *rq;
    struct task_struct *to_task;
    struct task_struct *from_task;
    uint64_t now_us;

    if (unlikely(!kernel_running)) {
        return;
    }

    if (unlikely(scheduler_lock_nest > 0U)) {
        struct sched_rq *locked_rq = this_rq();
        locked_rq->preempt_count = scheduler_lock_nest;
        locked_rq->need_resched = true;
        return;
    }

    rq = this_rq();
    rq->preempt_count = 0U;
    spin_lock_irq(&rq->lock);

    now_us = cpu_run_time_us();
    sys_heartbeat_time = now_us;
    from_task = rq->curr;

    if (from_task != NULL) {
        uint64_t delta_us = (now_us >= from_task->se.exec_start_us) ?
                            (now_us - from_task->se.exec_start_us) : 0ULL;
        bool is_idle = (from_task == rq->idle ||
                        from_task->policy == SCHED_POLICY_IDLE);

        sched_load_update_task(rq, from_task, now_us, delta_us, is_idle);
        if (from_task->sched_class != NULL &&
            from_task->sched_class->put_prev_task != NULL) {
            from_task->sched_class->put_prev_task(rq, from_task);
        }
    } else {
        rq->clock_us = now_us;
    }

#if CONFIG_NR_CPUS > 1
    if (rq->nr_running == 0U) {
        (void)sched_balance_rq_locked(rq);
    }
#endif

    to_task = pick_next_task_all(rq);
    rq->need_resched = false;

    if (to_task == NULL) {
        spin_unlock_irq(&rq->lock);
        return;
    }

    if (to_task != from_task) {
        if (from_task != NULL) {
            if (from_task->status == TASK_RUNNING) {
                from_task->status = TASK_READY;
                from_task->se.nivcsw++;
            } else if (from_task->status == TASK_READY) {
                from_task->se.nivcsw++;
            } else {
                from_task->se.nvcsw++;
            }
            from_task->on_cpu = 0U;
        }

        rq->nr_switches++;
        to_task->status = TASK_RUNNING;
        to_task->cpu = (uint8_t)rq->cpu_id;
        to_task->on_cpu = 1U;
        to_task->start_time = now_us;
        to_task->se.exec_start_us = now_us;
        rq->curr = to_task;

        hal_vm_switch(&to_task->vm_space);
        if (from_task != NULL) {
            context_switch((addr_t)&from_task->sp, (addr_t)&to_task->sp);
        } else {
            context_switch_to((addr_t)&to_task->sp);
        }
        spin_unlock_irq(&rq->lock);
        return;
    }

    to_task->status = TASK_RUNNING;
    to_task->on_cpu = 1U;
    to_task->start_time = now_us;
    to_task->se.exec_start_us = now_us;
    spin_unlock_irq(&rq->lock);
}

void sched_resched_check(void)
{
    struct sched_rq *rq;

    if (unlikely(!kernel_running)) {
        return;
    }
    rq = this_rq();
    rq->preempt_count = scheduler_lock_nest;
    if (scheduler_lock_nest == 0U && rq->need_resched) {
        switch_task();
    }
}

void add_task_to_ready_list_lock(struct task_struct *task)
{
    uint32_t target_cpu;
    struct sched_rq *rq;
    bool need_ipi = false;

    if (task == NULL) {
        pr_err("task is NULL\n");
        return;
    }

    target_cpu = sched_select_task_rq(task, SCHED_ENQUEUE_WAKEUP);
    rq = sched_rq_of(target_cpu);
    spin_lock_irq(&rq->lock);
    sched_enqueue_task_locked(rq, task, SCHED_ENQUEUE_WAKEUP);
    need_ipi = rq->need_resched;
    spin_unlock_irq(&rq->lock);
#if CONFIG_NR_CPUS > 1
    if (target_cpu != hal_cpu_id() && need_ipi) {
        hal_smp_send_ipi(target_cpu, NOS_IPI_RESCHED);
    }
#else
    (void)need_ipi;
#endif
}

void add_task_to_ready_list(struct task_struct *task)
{
    uint32_t target_cpu;
    struct sched_rq *rq;
    bool need_ipi = false;

    if (task == NULL) {
        pr_err("task is NULL\n");
        return;
    }

    spin_lock_irq(&task->lock);
    target_cpu = sched_select_task_rq(task, SCHED_ENQUEUE_WAKEUP);
    rq = sched_rq_of(target_cpu);
    spin_lock_irq(&rq->lock);
    sched_enqueue_task_locked(rq, task, SCHED_ENQUEUE_WAKEUP);
    need_ipi = rq->need_resched;
    spin_unlock_irq(&rq->lock);
    spin_unlock_irq(&task->lock);

#if CONFIG_NR_CPUS > 1
    if (target_cpu != hal_cpu_id() && need_ipi) {
        hal_smp_send_ipi(target_cpu, NOS_IPI_RESCHED);
    }
#else
    (void)need_ipi;
#endif
}

void del_task_to_ready_list_lock(struct task_struct *task)
{
    struct sched_rq *rq;

    if (task == NULL) {
        pr_err("task is NULL\n");
        return;
    }

    rq = sched_rq_of(task->cpu);
    spin_lock_irq(&rq->lock);
    sched_dequeue_task_locked(rq, task, SCHED_DEQUEUE_SLEEP);
    spin_unlock_irq(&rq->lock);
    task->status = TASK_SUSPENDED;
}

void del_task_to_ready_list(struct task_struct *task)
{
    struct sched_rq *rq;

    if (task == NULL) {
        pr_err("task is NULL\n");
        return;
    }

    spin_lock_irq(&task->lock);
    rq = sched_rq_of(task->cpu);
    spin_lock_irq(&rq->lock);
    sched_dequeue_task_locked(rq, task, SCHED_DEQUEUE_SLEEP);
    spin_unlock_irq(&rq->lock);
    task->status = TASK_SUSPENDED;
    spin_unlock_irq(&task->lock);
}

uint32_t get_sch_lock_level(void)
{
    return scheduler_lock_nest;
}

void sch_heartbeat(void)
{
    struct sched_rq *rq;
    struct task_struct *curr;
    uint64_t now_us;

    calculate_sys_cycle();
    rq = this_rq();
    now_us = cpu_run_time_us();
    sys_heartbeat_time = now_us;

    spin_lock_irq(&rq->lock);
    curr = rq->curr;
    if (curr != NULL) {
        uint64_t delta_us;
        bool is_idle = (curr == rq->idle || curr->policy == SCHED_POLICY_IDLE);

        if (now_us > curr->se.exec_start_us) {
            delta_us = now_us - curr->se.exec_start_us;
        } else {
#ifdef CONFIG_SYS_TICK_MS
            delta_us = (uint64_t)CONFIG_SYS_TICK_MS * 1000ULL;
#else
            delta_us = 1000ULL;
#endif
        }

        sched_load_update_task(rq, curr, now_us, delta_us, is_idle);
        if (curr->sched_class == NULL) {
            curr->sched_class = sched_policy_to_class(curr->policy);
        }
        if (curr->sched_class->task_tick != NULL) {
            curr->sched_class->task_tick(rq, curr);
        }
    }
    sched_load_tick(rq, now_us);
    spin_unlock_irq(&rq->lock);

    if (rq->need_resched && scheduler_lock_nest == 0U) {
        switch_task();
    }
}

uint64_t sched_get_total_switches(void)
{
    uint64_t total = 0ULL;
    uint32_t i;

    for (i = 0U; i < CONFIG_NR_CPUS; i++) {
        total += g_sched_rqs[i].nr_switches;
    }
    return total;
}
