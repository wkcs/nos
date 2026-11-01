/**
 * Copyright (C) 2023-2026 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <nos/sched.h>
#include <kernel/cpu.h>
#include <string.h>

#define SCHED_RT_DEFAULT_TICK 10U

static void rt_sync_task_mask(struct task_struct *p)
{
    uint8_t prio = p->current_priority;

#if CONFIG_MAX_PRIORITY < 256
    if (prio >= CONFIG_MAX_PRIORITY) {
        prio = CONFIG_MAX_PRIORITY - 1U;
        p->current_priority = prio;
    }
#endif
#if CONFIG_MAX_PRIORITY > 32
    p->offset = (uint8_t)(prio >> 3);
    p->offset_mask = 1UL << p->offset;
    p->prio_mask = (uint8_t)(1U << (prio & 0x07U));
#else
    p->offset_mask = 1UL << prio;
#endif
}

static uint16_t rt_find_highest_prio(const struct rt_rq *rt)
{
    if (rt == NULL || rt->prio_grp == 0U) {
        return (uint16_t)CONFIG_MAX_PRIORITY;
    }
#if CONFIG_MAX_PRIORITY > 32
    {
        uint32_t offset = (uint32_t)(__ffs((int)rt->prio_grp) - 1);
        uint32_t bit = (uint32_t)(__ffs((int)rt->prio_tbl[offset]) - 1);

        return (uint16_t)((offset << 3) + bit);
    }
#else
    return (uint16_t)(__ffs((int)rt->prio_grp) - 1);
#endif
}

void sched_rt_rq_init(struct rt_rq *rt)
{
    uint32_t i;

    if (rt == NULL) {
        return;
    }

    for (i = 0U; i < CONFIG_MAX_PRIORITY; i++) {
        INIT_LIST_HEAD(&rt->queue[i]);
    }
    rt->prio_grp = 0U;
#if CONFIG_MAX_PRIORITY > 32
    memset(rt->prio_tbl, 0, sizeof(rt->prio_tbl));
#endif
    rt->nr_running = 0U;
    rt->highest_prio = (uint16_t)CONFIG_MAX_PRIORITY;
}

static void rt_enqueue_task(struct sched_rq *rq, struct task_struct *p, int flags)
{
    uint8_t prio;

    (void)flags;
    if (rq == NULL || p == NULL || p->se.on_rq) {
        return;
    }

    rt_sync_task_mask(p);
    prio = p->current_priority;

    list_add_tail(&p->se.run_node, &rq->rt.queue[prio]);
#if CONFIG_MAX_PRIORITY > 32
    rq->rt.prio_tbl[p->offset] |= p->prio_mask;
#endif
    rq->rt.prio_grp |= p->offset_mask;

    rq->rt.nr_running++;
    rq->nr_running++;
    p->se.on_rq = true;

    if (prio < rq->rt.highest_prio) {
        rq->rt.highest_prio = prio;
    }
    if (p->remaining_tick == 0U) {
        p->remaining_tick = (p->init_tick != 0U) ? p->init_tick : SCHED_RT_DEFAULT_TICK;
    }
}

static void rt_dequeue_task(struct sched_rq *rq, struct task_struct *p, int flags)
{
    uint8_t prio;

    (void)flags;
    if (rq == NULL || p == NULL || !p->se.on_rq) {
        return;
    }

    rt_sync_task_mask(p);
    prio = p->current_priority;

    list_del_init(&p->se.run_node);
    if (list_empty(&rq->rt.queue[prio])) {
#if CONFIG_MAX_PRIORITY > 32
        rq->rt.prio_tbl[p->offset] &= (uint8_t)(~p->prio_mask);
        if (rq->rt.prio_tbl[p->offset] == 0U) {
            rq->rt.prio_grp &= ~p->offset_mask;
        }
#else
        rq->rt.prio_grp &= ~p->offset_mask;
#endif
    }

    if (rq->rt.nr_running > 0U) {
        rq->rt.nr_running--;
    }
    if (rq->nr_running > 0U) {
        rq->nr_running--;
    }
    p->se.on_rq = false;
    rq->rt.highest_prio = rt_find_highest_prio(&rq->rt);
}

static void rt_yield_task(struct sched_rq *rq, struct task_struct *p)
{
    uint8_t prio;

    if (rq == NULL || p == NULL) {
        return;
    }

    prio = p->current_priority;
    if (p->se.on_rq && prio < CONFIG_MAX_PRIORITY &&
        !list_is_singular(&rq->rt.queue[prio])) {
        list_move_tail(&p->se.run_node, &rq->rt.queue[prio]);
        p->remaining_tick = (p->init_tick != 0U) ? p->init_tick : SCHED_RT_DEFAULT_TICK;
        rq->need_resched = true;
    }
}

static void rt_check_preempt_curr(struct sched_rq *rq, struct task_struct *p, int flags)
{
    (void)flags;
    if (rq == NULL || p == NULL) {
        return;
    }

    if (rq->curr == NULL ||
        rq->curr == rq->idle ||
        rq->curr->policy == SCHED_POLICY_NORMAL ||
        rq->curr->policy == SCHED_POLICY_IDLE ||
        p->current_priority < rq->curr->current_priority) {
        rq->need_resched = true;
    }
}

static struct task_struct *rt_pick_next_task(struct sched_rq *rq)
{
    uint16_t prio;

    if (rq == NULL || rq->rt.nr_running == 0U || rq->rt.prio_grp == 0U) {
        return NULL;
    }

    prio = rt_find_highest_prio(&rq->rt);
    if (prio >= CONFIG_MAX_PRIORITY || list_empty(&rq->rt.queue[prio])) {
        return NULL;
    }

    return list_first_entry(&rq->rt.queue[prio], struct task_struct, se.run_node);
}

static void rt_put_prev_task(struct sched_rq *rq, struct task_struct *prev)
{
    (void)rq;
    (void)prev;
}

static void rt_task_tick(struct sched_rq *rq, struct task_struct *curr)
{
    uint8_t prio;

    if (rq == NULL || curr == NULL) {
        return;
    }

    if (rq->rt.highest_prio < curr->current_priority) {
        rq->need_resched = true;
    }

    if (curr->policy == SCHED_POLICY_FIFO) {
        return;
    }

    if (curr->remaining_tick > 0U) {
        curr->remaining_tick--;
    }
    if (curr->remaining_tick == 0U) {
        curr->remaining_tick = (curr->init_tick != 0U) ? curr->init_tick : SCHED_RT_DEFAULT_TICK;
        prio = curr->current_priority;
        if (curr->se.on_rq && prio < CONFIG_MAX_PRIORITY &&
            !list_is_singular(&rq->rt.queue[prio])) {
            list_move_tail(&curr->se.run_node, &rq->rt.queue[prio]);
            rq->need_resched = true;
        }
    }
}

const struct sched_class rt_sched_class = {
    .name               = "rt",
#ifdef CONFIG_SCHED_FAIR
    .next               = &fair_sched_class,
#else
    .next               = &idle_sched_class,
#endif
    .enqueue_task       = rt_enqueue_task,
    .dequeue_task       = rt_dequeue_task,
    .yield_task         = rt_yield_task,
    .check_preempt_curr = rt_check_preempt_curr,
    .pick_next_task     = rt_pick_next_task,
    .put_prev_task      = rt_put_prev_task,
    .task_tick          = rt_task_tick,
    .select_task_rq     = NULL,
    .balance            = NULL,
};
