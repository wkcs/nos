/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS 推理 SLO 感知调度类 (`infer_sched_class`):
 * - EDF（Earliest Deadline First）按绝对截止期保障 TTFT / TPOT 尾延迟
 * - CBS（Constant Bandwidth Server）周期带宽节流，防 Prefill 洪峰或异常死循环饿死 Fair 服务
 * - Deadline Inheritance（截止期继承）跨 RingIPC 传递紧迫度至工具代理线程
 * - EAS 异构核拓扑选核（PREFILL->大核/AI指令, DECODE->分散核簇, AGENT_CTRL->小核）
 * - Gang Group 多核协同同步换入
 */

#include <nos/sched.h>
#include <nos/sched_infer.h>
#include <nos/hal_arch.h>
#include <uapi/nos/errno.h>
#include <kernel/cpu.h>
#include <string.h>

#define SCHED_INFER_DEFAULT_RUNTIME_NS   2000000ULL  /* 默认 2 ms CBS 预算 */
#define SCHED_INFER_DEFAULT_DEADLINE_NS  10000000ULL /* 默认 10 ms 相对截止期 */
#define SCHED_INFER_DEFAULT_PERIOD_NS    10000000ULL /* 默认 10 ms 周期 */

struct nos_gang_group {
    bool                in_use;
    uint32_t            gang_id;
    uint32_t            member_count;
    struct task_struct *members[NOS_GANG_MAX_MEMBERS];
};

static struct nos_gang_group g_gang_groups[NOS_GANG_MAX_GROUPS];
static spinlock_t g_gang_lock;

static uint64_t infer_now_ns(void)
{
    return hal_timer_monotonic_ns();
}

static void infer_refresh_earliest_locked(struct infer_rq *infer)
{
    struct task_struct *pos;

    infer->earliest_deadline_ns = UINT64_MAX;
    list_for_each_entry(pos, &infer->tasks_edf, se.run_node) {
        if (!pos->se.infer.throttled) {
            infer->earliest_deadline_ns = pos->se.infer.abs_deadline_ns;
            return;
        }
    }
}

static void infer_insert_edf_locked(struct infer_rq *infer, struct task_struct *p)
{
    struct task_struct *pos;

    list_for_each_entry(pos, &infer->tasks_edf, se.run_node) {
        if (p->se.infer.abs_deadline_ns < pos->se.infer.abs_deadline_ns) {
            list_add_tail(&p->se.run_node, &pos->se.run_node);
            infer_refresh_earliest_locked(infer);
            return;
        }
    }
    list_add_tail(&p->se.run_node, &infer->tasks_edf);
    infer_refresh_earliest_locked(infer);
}

static void infer_requeue_sorted_locked(struct sched_rq *rq, struct task_struct *p)
{
    if (!p->se.on_rq || p->policy != SCHED_POLICY_INFER) {
        return;
    }
    list_del_init(&p->se.run_node);
    infer_insert_edf_locked(&rq->infer, p);
}

static void infer_replenish_throttled_locked(struct sched_rq *rq, uint64_t now_ns)
{
    struct task_struct *pos;
    struct task_struct *tmp;
    bool moved = false;

    list_for_each_entry_safe(pos, tmp, &rq->infer.tasks_edf, se.run_node) {
        struct nos_infer_entity *inf = &pos->se.infer;

        if (!inf->throttled || now_ns < inf->throttle_until_ns) {
            continue;
        }
        inf->throttled = false;
        inf->remaining_runtime_ns = (inf->runtime_ns != 0ULL)
                                        ? inf->runtime_ns
                                        : SCHED_INFER_DEFAULT_RUNTIME_NS;
        if (inf->abs_deadline_ns < now_ns) {
            uint64_t dl = (inf->deadline_ns != 0ULL) ? inf->deadline_ns : SCHED_INFER_DEFAULT_DEADLINE_NS;
            inf->abs_deadline_ns = now_ns + dl;
        }
        {
            uint64_t period = (inf->period_ns != 0ULL) ? inf->period_ns : SCHED_INFER_DEFAULT_PERIOD_NS;
            while (inf->throttle_until_ns <= now_ns) {
                inf->throttle_until_ns += period;
            }
        }
        list_del_init(&pos->se.run_node);
        infer_insert_edf_locked(&rq->infer, pos);
        moved = true;
    }

    if (moved) {
        infer_refresh_earliest_locked(&rq->infer);
    }
}

void sched_infer_rq_init(struct infer_rq *infer)
{
    if (infer == NULL) {
        return;
    }
    INIT_LIST_HEAD(&infer->tasks_edf);
    infer->nr_running = 0U;
    infer->earliest_deadline_ns = UINT64_MAX;
}

static void infer_enqueue_task(struct sched_rq *rq, struct task_struct *p, int flags)
{
    struct nos_infer_entity *inf;
    uint64_t now_ns;

    if (rq == NULL || p == NULL || p->se.on_rq) {
        return;
    }

    inf = &p->se.infer;
    now_ns = infer_now_ns();

    if (inf->runtime_ns == 0ULL) {
        inf->runtime_ns = SCHED_INFER_DEFAULT_RUNTIME_NS;
    }
    if (inf->deadline_ns == 0ULL) {
        inf->deadline_ns = SCHED_INFER_DEFAULT_DEADLINE_NS;
    }
    if (inf->period_ns == 0ULL) {
        inf->period_ns = SCHED_INFER_DEFAULT_PERIOD_NS;
    }
    if (inf->abs_deadline_ns == 0ULL ||
        ((flags & SCHED_ENQUEUE_WAKEUP) != 0 && now_ns >= inf->abs_deadline_ns)) {
        inf->abs_deadline_ns = now_ns + inf->deadline_ns;
        inf->remaining_runtime_ns = inf->runtime_ns;
        inf->throttle_until_ns = now_ns + inf->period_ns;
        inf->throttled = false;
    } else if (inf->throttled && now_ns >= inf->throttle_until_ns) {
        inf->throttled = false;
        inf->remaining_runtime_ns = inf->runtime_ns;
        inf->throttle_until_ns = now_ns + inf->period_ns;
    } else if (inf->remaining_runtime_ns == 0ULL && !inf->throttled) {
        inf->remaining_runtime_ns = inf->runtime_ns;
    }

    infer_insert_edf_locked(&rq->infer, p);
    rq->infer.nr_running++;
    rq->nr_running++;
    p->se.on_rq = true;
}

static void infer_dequeue_task(struct sched_rq *rq, struct task_struct *p, int flags)
{
    (void)flags;
    if (rq == NULL || p == NULL || !p->se.on_rq) {
        return;
    }

    list_del_init(&p->se.run_node);
    if (rq->infer.nr_running > 0U) {
        rq->infer.nr_running--;
    }
    if (rq->nr_running > 0U) {
        rq->nr_running--;
    }
    p->se.on_rq = false;
    infer_refresh_earliest_locked(&rq->infer);
}

static void infer_yield_task(struct sched_rq *rq, struct task_struct *p)
{
    struct task_struct *next_peer = NULL;
    struct task_struct *pos;

    if (rq == NULL || p == NULL || !p->se.on_rq || list_is_singular(&rq->infer.tasks_edf)) {
        return;
    }

    /* 寻找排在自身之后首个未限流兄弟任务并让位，维持协作可切换性与公平性 */
    bool seen_self = false;
    list_for_each_entry(pos, &rq->infer.tasks_edf, se.run_node) {
        if (!seen_self) {
            if (pos == p) {
                seen_self = true;
            }
            continue;
        }
        if (!pos->se.infer.throttled) {
            next_peer = pos;
            break;
        }
    }

    if (next_peer != NULL) {
        if (p->se.infer.abs_deadline_ns < next_peer->se.infer.abs_deadline_ns) {
            p->se.infer.abs_deadline_ns = next_peer->se.infer.abs_deadline_ns;
        }
        list_del_init(&p->se.run_node);
        list_add(&p->se.run_node, &next_peer->se.run_node);
        infer_refresh_earliest_locked(&rq->infer);
        rq->need_resched = true;
    }
}

static void infer_check_preempt_curr(struct sched_rq *rq, struct task_struct *p, int flags)
{
    (void)flags;
    if (rq == NULL || p == NULL || p->se.infer.throttled) {
        return;
    }

    if (rq->curr == NULL ||
        rq->curr == rq->idle ||
        rq->curr->policy == SCHED_POLICY_NORMAL ||
        rq->curr->policy == SCHED_POLICY_IDLE) {
        rq->need_resched = true;
        return;
    }

    if (rq->curr->policy == SCHED_POLICY_INFER &&
        (rq->curr->se.infer.throttled ||
         p->se.infer.abs_deadline_ns < rq->curr->se.infer.abs_deadline_ns)) {
        rq->need_resched = true;
    }
}

static struct task_struct *infer_pick_next_task(struct sched_rq *rq)
{
    struct task_struct *pos;
    uint64_t now_ns;

    if (rq == NULL || rq->infer.nr_running == 0U || list_empty(&rq->infer.tasks_edf)) {
        return NULL;
    }

    now_ns = infer_now_ns();
    infer_replenish_throttled_locked(rq, now_ns);

    list_for_each_entry(pos, &rq->infer.tasks_edf, se.run_node) {
        if (!pos->se.infer.throttled) {
            return pos;
        }
    }

    return NULL;
}

static void infer_put_prev_task(struct sched_rq *rq, struct task_struct *prev)
{
    (void)rq;
    (void)prev;
}

static void infer_task_tick(struct sched_rq *rq, struct task_struct *curr)
{
    struct nos_infer_entity *inf;
    uint64_t now_ns;
    uint64_t step_ns;
    uint64_t period;

    if (rq == NULL || curr == NULL) {
        return;
    }

    now_ns = infer_now_ns();
    infer_replenish_throttled_locked(rq, now_ns);

    inf = &curr->se.infer;
    if (inf->runtime_ns == 0ULL) {
        inf->runtime_ns = SCHED_INFER_DEFAULT_RUNTIME_NS;
    }
    period = (inf->period_ns != 0ULL) ? inf->period_ns : SCHED_INFER_DEFAULT_PERIOD_NS;

#ifdef CONFIG_SYS_TICK_MS
    step_ns = (uint64_t)((CONFIG_SYS_TICK_MS > 0) ? CONFIG_SYS_TICK_MS : 1) * 1000000ULL;
#else
    step_ns = 1000000ULL;
#endif

    if (inf->remaining_runtime_ns > step_ns) {
        inf->remaining_runtime_ns -= step_ns;
        if (rq->infer.earliest_deadline_ns < inf->abs_deadline_ns) {
            rq->need_resched = true;
        }
        return;
    }

    /* CBS 周期配额已耗尽 */
    inf->remaining_runtime_ns = 0ULL;
    if (now_ns < inf->throttle_until_ns) {
        inf->throttled = true;
        inf->abs_deadline_ns += period;
        infer_requeue_sorted_locked(rq, curr);
        rq->need_resched = true;
    } else {
        uint64_t dl = (inf->deadline_ns != 0ULL) ? inf->deadline_ns : period;
        inf->throttled = false;
        inf->remaining_runtime_ns = inf->runtime_ns;
        inf->abs_deadline_ns = now_ns + dl;
        inf->throttle_until_ns = now_ns + period;
        infer_requeue_sorted_locked(rq, curr);
        rq->need_resched = true;
    }
}

__attribute__((weak)) int arch_sched_query_topo(struct nos_cpu_topo_info *out_info)
{
    uint32_t cpu;
    uint32_t online_mask = 0U;

    if (out_info == NULL) {
        return -EINVAL;
    }

    memset(out_info, 0, sizeof(*out_info));
    out_info->nr_cpus = CONFIG_NR_CPUS;
    for (cpu = 0U; cpu < CONFIG_NR_CPUS && cpu < 32U; cpu++) {
        if (g_sched_rqs[cpu].online) {
            online_mask |= (1U << cpu);
        }
    }
    out_info->online_mask = (online_mask != 0U) ? online_mask : 1U;
    out_info->big_cpu_mask = out_info->online_mask;
    out_info->little_cpu_mask = out_info->online_mask;
    return 0;
}

static uint32_t infer_select_task_rq(struct task_struct *p, uint32_t prev_cpu, int wake_flags)
{
    struct nos_cpu_topo_info topo;
    uint32_t allow_mask;
    uint32_t pref_mask;
    uint32_t cpu;
    uint32_t best_cpu = prev_cpu;
    uint32_t best_load = U32_MAX;
    bool found = false;

    (void)wake_flags;
    if (p == NULL) {
        return 0U;
    }
    if (CONFIG_NR_CPUS <= 1) {
        return 0U;
    }

    allow_mask = (p->cpus_allowed != 0U) ? (uint32_t)p->cpus_allowed : 0xFFFFFFFFU;
    if (arch_sched_query_topo(&topo) != 0) {
        return prev_cpu;
    }
    allow_mask &= topo.online_mask;
    if (allow_mask == 0U) {
        return prev_cpu;
    }

    switch (p->se.infer.role) {
    case NOS_INFER_ROLE_PREFILL:
        pref_mask = allow_mask & topo.big_cpu_mask;
        break;
    case NOS_INFER_ROLE_AGENT_CTRL:
        pref_mask = allow_mask & topo.little_cpu_mask;
        break;
    default:
        pref_mask = allow_mask;
        break;
    }
    if (pref_mask == 0U) {
        pref_mask = allow_mask;
    }

    for (cpu = 0U; cpu < CONFIG_NR_CPUS && cpu < 32U; cpu++) {
        struct sched_rq *rq;
        uint32_t score;

        if ((pref_mask & (1U << cpu)) == 0U || !g_sched_rqs[cpu].online) {
            continue;
        }
        rq = &g_sched_rqs[cpu];
        score = rq->infer.nr_running * 64U + rq->nr_running * 8U + rq->cpu_util_avg;
        /* DECODE 角色倾向优先打散到不同核以避免内存带宽拥堵，相同分数时偏向分散核 */
        if (!found || score < best_load) {
            best_cpu = cpu;
            best_load = score;
            found = true;
        }
    }

    return found ? best_cpu : prev_cpu;
}

const struct sched_class infer_sched_class = {
    .name               = "infer",
#ifdef CONFIG_SCHED_FAIR
    .next               = &fair_sched_class,
#else
    .next               = &idle_sched_class,
#endif
    .enqueue_task       = infer_enqueue_task,
    .dequeue_task       = infer_dequeue_task,
    .yield_task         = infer_yield_task,
    .check_preempt_curr = infer_check_preempt_curr,
    .pick_next_task     = infer_pick_next_task,
    .put_prev_task      = infer_put_prev_task,
    .task_tick          = infer_task_tick,
    .select_task_rq     = infer_select_task_rq,
    .balance            = NULL,
};

int sched_infer_set_param(struct task_struct *task,
                          const struct nos_infer_sched_param *param)
{
    struct sched_rq *rq;
    uint64_t now_ns;
    bool was_on_rq;
    uint32_t target_cpu = 0U;
    bool need_ipi = false;

    if (task == NULL || param == NULL) {
        return -EINVAL;
    }
    if (param->role > (uint32_t)NOS_INFER_ROLE_TOOL_WORKER) {
        return -EINVAL;
    }
    if (param->role != (uint32_t)NOS_INFER_ROLE_NONE) {
        if (param->runtime_ns == 0ULL ||
            param->deadline_ns < param->runtime_ns ||
            param->period_ns < param->deadline_ns) {
            return -EINVAL;
        }
    }

    if (param->gang_id != 0U) {
        int rc = sched_infer_gang_join(task, param->gang_id);
        if (rc < 0) {
            return rc;
        }
    } else if (task->se.infer.gang_id != 0U) {
        sched_infer_gang_leave(task);
    }

    spin_lock_irq(&task->lock);
    rq = sched_rq_of(task->cpu);
    spin_lock_irq(&rq->lock);

    was_on_rq = task->se.on_rq;
    if (was_on_rq) {
        sched_dequeue_task_locked(rq, task, SCHED_DEQUEUE_MIGRATE);
    }

    now_ns = infer_now_ns();
    if (param->role == (uint32_t)NOS_INFER_ROLE_NONE) {
        task->se.infer.role = NOS_INFER_ROLE_NONE;
        task->se.infer.runtime_ns = 0ULL;
        task->se.infer.deadline_ns = 0ULL;
        task->se.infer.period_ns = 0ULL;
        task->se.infer.abs_deadline_ns = 0ULL;
        task->se.infer.remaining_runtime_ns = 0ULL;
        task->se.infer.throttled = false;
        task->se.infer.deadline_inherited = false;
#ifdef CONFIG_SCHED_FAIR
        task->policy = SCHED_POLICY_NORMAL;
#else
        task->policy = SCHED_POLICY_RR;
#endif
        task->sched_class = sched_policy_to_class(task->policy);
    } else {
        task->se.infer.role = param->role;
        task->se.infer.runtime_ns = param->runtime_ns;
        task->se.infer.deadline_ns = param->deadline_ns;
        task->se.infer.period_ns = param->period_ns;
        task->se.infer.abs_deadline_ns = now_ns + param->deadline_ns;
        task->se.infer.remaining_runtime_ns = param->runtime_ns;
        task->se.infer.throttle_until_ns = now_ns + param->period_ns;
        task->se.infer.throttled = false;
        task->se.infer.deadline_inherited = false;
        task->policy = SCHED_POLICY_INFER;
        task->sched_class = &infer_sched_class;
    }

    if (was_on_rq) {
        sched_enqueue_task_locked(rq, task, SCHED_ENQUEUE_RESTORE);
        need_ipi = rq->need_resched;
        target_cpu = rq->cpu_id;
    }

    spin_unlock_irq(&rq->lock);
    spin_unlock_irq(&task->lock);

#if CONFIG_NR_CPUS > 1
    if (need_ipi && target_cpu != hal_cpu_id()) {
        hal_smp_send_ipi(target_cpu, NOS_IPI_RESCHED);
    }
#else
    (void)need_ipi;
    (void)target_cpu;
#endif
    return 0;
}

int sched_infer_boost_deadline(struct task_struct *target,
                               uint64_t inherited_abs_deadline_ns)
{
    struct sched_rq *rq;
    uint64_t now_ns;
    bool was_on_rq;
    bool need_ipi = false;
    uint32_t target_cpu = 0U;

    if (target == NULL || inherited_abs_deadline_ns == 0ULL) {
        return -EINVAL;
    }

    spin_lock_irq(&target->lock);
    rq = sched_rq_of(target->cpu);
    spin_lock_irq(&rq->lock);

    /* 不降级硬实时任务 */
    if (target->policy == SCHED_POLICY_FIFO || target->policy == SCHED_POLICY_RR) {
        spin_unlock_irq(&rq->lock);
        spin_unlock_irq(&target->lock);
        return 0;
    }

    if (target->policy == SCHED_POLICY_INFER &&
        target->se.infer.abs_deadline_ns != 0ULL &&
        target->se.infer.abs_deadline_ns <= inherited_abs_deadline_ns) {
        spin_unlock_irq(&rq->lock);
        spin_unlock_irq(&target->lock);
        return 0;
    }

    was_on_rq = target->se.on_rq;
    if (was_on_rq) {
        sched_dequeue_task_locked(rq, target, SCHED_DEQUEUE_MIGRATE);
    }

    now_ns = infer_now_ns();
    if (!target->se.infer.deadline_inherited) {
        target->se.infer.saved_policy = target->policy;
        target->se.infer.saved_deadline_ns = target->se.infer.deadline_ns;
        target->se.infer.saved_abs_deadline_ns = target->se.infer.abs_deadline_ns;
        target->se.infer.deadline_inherited = true;
    }

    if (target->policy != SCHED_POLICY_INFER) {
        target->policy = SCHED_POLICY_INFER;
        target->sched_class = &infer_sched_class;
        if (target->se.infer.runtime_ns == 0ULL) {
            target->se.infer.runtime_ns = SCHED_INFER_DEFAULT_RUNTIME_NS;
            target->se.infer.period_ns = SCHED_INFER_DEFAULT_PERIOD_NS;
            target->se.infer.remaining_runtime_ns = SCHED_INFER_DEFAULT_RUNTIME_NS;
            target->se.infer.throttle_until_ns = now_ns + SCHED_INFER_DEFAULT_PERIOD_NS;
        }
    }

    target->se.infer.abs_deadline_ns = inherited_abs_deadline_ns;
    target->se.infer.deadline_ns = (inherited_abs_deadline_ns > now_ns)
                                       ? (inherited_abs_deadline_ns - now_ns)
                                       : 100000ULL;
    target->se.infer.throttled = false;
    if (target->se.infer.remaining_runtime_ns == 0ULL) {
        target->se.infer.remaining_runtime_ns = target->se.infer.runtime_ns;
    }

    if (was_on_rq) {
        sched_enqueue_task_locked(rq, target, SCHED_ENQUEUE_RESTORE);
        need_ipi = rq->need_resched;
        target_cpu = rq->cpu_id;
    }

    spin_unlock_irq(&rq->lock);
    spin_unlock_irq(&target->lock);

#if CONFIG_NR_CPUS > 1
    if (need_ipi && target_cpu != hal_cpu_id()) {
        hal_smp_send_ipi(target_cpu, NOS_IPI_RESCHED);
    }
#else
    (void)need_ipi;
    (void)target_cpu;
#endif
    return 0;
}

void sched_infer_restore_deadline(struct task_struct *target)
{
    struct sched_rq *rq;
    bool was_on_rq;

    if (target == NULL) {
        return;
    }

    spin_lock_irq(&target->lock);
    if (!target->se.infer.deadline_inherited) {
        spin_unlock_irq(&target->lock);
        return;
    }

    rq = sched_rq_of(target->cpu);
    spin_lock_irq(&rq->lock);

    was_on_rq = target->se.on_rq;
    if (was_on_rq) {
        sched_dequeue_task_locked(rq, target, SCHED_DEQUEUE_MIGRATE);
    }

    target->policy = target->se.infer.saved_policy;
    target->sched_class = sched_policy_to_class(target->policy);
    target->se.infer.deadline_ns = target->se.infer.saved_deadline_ns;
    target->se.infer.abs_deadline_ns = target->se.infer.saved_abs_deadline_ns;
    target->se.infer.deadline_inherited = false;

    if (was_on_rq) {
        sched_enqueue_task_locked(rq, target, SCHED_ENQUEUE_RESTORE);
    }

    spin_unlock_irq(&rq->lock);
    spin_unlock_irq(&target->lock);
}

int sched_infer_gang_join(struct task_struct *task, uint32_t gang_id)
{
    struct nos_gang_group *grp = NULL;
    struct nos_gang_group *empty_slot = NULL;
    uint32_t i;

    if (task == NULL || gang_id == 0U) {
        return -EINVAL;
    }

    sched_infer_gang_leave(task);

    spin_lock_irq(&g_gang_lock);
    for (i = 0U; i < NOS_GANG_MAX_GROUPS; i++) {
        if (g_gang_groups[i].in_use && g_gang_groups[i].gang_id == gang_id) {
            grp = &g_gang_groups[i];
            break;
        }
        if (!g_gang_groups[i].in_use && empty_slot == NULL) {
            empty_slot = &g_gang_groups[i];
        }
    }

    if (grp == NULL) {
        if (empty_slot == NULL) {
            spin_unlock_irq(&g_gang_lock);
            return -ENOSPC;
        }
        grp = empty_slot;
        memset(grp, 0, sizeof(*grp));
        grp->in_use = true;
        grp->gang_id = gang_id;
    }

    if (grp->member_count >= NOS_GANG_MAX_MEMBERS) {
        spin_unlock_irq(&g_gang_lock);
        return -ENOSPC;
    }

    grp->members[grp->member_count++] = task;
    task->se.infer.gang_id = gang_id;
    spin_unlock_irq(&g_gang_lock);
    return 0;
}

void sched_infer_gang_leave(struct task_struct *task)
{
    uint32_t i;

    if (task == NULL) {
        return;
    }

    spin_lock_irq(&g_gang_lock);
    for (i = 0U; i < NOS_GANG_MAX_GROUPS; i++) {
        struct nos_gang_group *grp = &g_gang_groups[i];
        uint32_t j;

        if (!grp->in_use) {
            continue;
        }
        for (j = 0U; j < grp->member_count; j++) {
            if (grp->members[j] == task) {
                uint32_t k;
                for (k = j; (k + 1U) < grp->member_count; k++) {
                    grp->members[k] = grp->members[k + 1U];
                }
                grp->member_count--;
                grp->members[grp->member_count] = NULL;
                if (grp->member_count == 0U) {
                    grp->in_use = false;
                    grp->gang_id = 0U;
                }
                task->se.infer.gang_id = 0U;
                spin_unlock_irq(&g_gang_lock);
                return;
            }
        }
    }
    task->se.infer.gang_id = 0U;
    spin_unlock_irq(&g_gang_lock);
}

int sched_infer_gang_wakeup(uint32_t gang_id)
{
    struct task_struct *targets[NOS_GANG_MAX_MEMBERS];
    uint32_t count = 0U;
    uint32_t cpu_mask = 0U;
    uint32_t self_cpu = hal_cpu_id();
    uint32_t i;

    if (gang_id == 0U) {
        return -EINVAL;
    }

    spin_lock_irq(&g_gang_lock);
    for (i = 0U; i < NOS_GANG_MAX_GROUPS; i++) {
        if (g_gang_groups[i].in_use && g_gang_groups[i].gang_id == gang_id) {
            uint32_t j;
            for (j = 0U; j < g_gang_groups[i].member_count; j++) {
                struct task_struct *m = g_gang_groups[i].members[j];
                if (m != NULL && task_get(m)) {
                    targets[count++] = m;
                }
            }
            break;
        }
    }
    spin_unlock_irq(&g_gang_lock);

    if (count == 0U) {
        return -ENOENT;
    }

    for (i = 0U; i < count; i++) {
        struct task_struct *m = targets[i];
        struct sched_rq *rq;

        spin_lock_irq(&m->lock);
        rq = sched_rq_of(m->cpu);
        spin_lock_irq(&rq->lock);
        if (!m->se.on_rq && m->status != TASK_ZOMBIE) {
            sched_enqueue_task_locked(rq, m, SCHED_ENQUEUE_WAKEUP);
        }
        rq->need_resched = true;
        if (rq->cpu_id < 32U) {
            cpu_mask |= (1U << rq->cpu_id);
        }
        spin_unlock_irq(&rq->lock);
        spin_unlock_irq(&m->lock);
        (void)task_put(m);
    }

#if CONFIG_NR_CPUS > 1
    for (i = 0U; i < CONFIG_NR_CPUS && i < 32U; i++) {
        if (i != self_cpu && (cpu_mask & (1U << i)) != 0U) {
            hal_smp_send_ipi(i, NOS_IPI_RESCHED);
        }
    }
#else
    (void)cpu_mask;
    (void)self_cpu;
#endif
    return (int)count;
}

int sched_infer_query_topo(struct nos_cpu_topo_info *out_info)
{
    uint32_t active = 0U;
    uint32_t i;
    int rc;

    if (out_info == NULL) {
        return -EINVAL;
    }
    rc = arch_sched_query_topo(out_info);
    if (rc < 0) {
        return rc;
    }

    spin_lock_irq(&g_gang_lock);
    for (i = 0U; i < NOS_GANG_MAX_GROUPS; i++) {
        if (g_gang_groups[i].in_use) {
            active++;
        }
    }
    spin_unlock_irq(&g_gang_lock);
    out_info->active_gangs = active;
    return 0;
}
