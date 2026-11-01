/**
 * Copyright (C) 2023-2026 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <nos/sched.h>

static void idle_enqueue_task(struct sched_rq *rq, struct task_struct *p, int flags)
{
    (void)flags;
    if (rq == NULL || p == NULL) {
        return;
    }
    if (rq->idle == NULL) {
        rq->idle = p;
    }
    p->se.on_rq = true;
}

static void idle_dequeue_task(struct sched_rq *rq, struct task_struct *p, int flags)
{
    (void)rq;
    (void)flags;
    if (p != NULL) {
        p->se.on_rq = false;
    }
}

static void idle_yield_task(struct sched_rq *rq, struct task_struct *p)
{
    (void)p;
    if (rq != NULL) {
        rq->need_resched = true;
    }
}

static void idle_check_preempt_curr(struct sched_rq *rq, struct task_struct *p, int flags)
{
    (void)rq;
    (void)p;
    (void)flags;
}

static struct task_struct *idle_pick_next_task(struct sched_rq *rq)
{
    if (rq == NULL) {
        return NULL;
    }
    return rq->idle;
}

static void idle_put_prev_task(struct sched_rq *rq, struct task_struct *prev)
{
    (void)rq;
    (void)prev;
}

static void idle_task_tick(struct sched_rq *rq, struct task_struct *curr)
{
    (void)curr;
    if (rq != NULL && rq->nr_running > 0U) {
        rq->need_resched = true;
    }
}

const struct sched_class idle_sched_class = {
    .name               = "idle",
    .next               = NULL,
    .enqueue_task       = idle_enqueue_task,
    .dequeue_task       = idle_dequeue_task,
    .yield_task         = idle_yield_task,
    .check_preempt_curr = idle_check_preempt_curr,
    .pick_next_task     = idle_pick_next_task,
    .put_prev_task      = idle_put_prev_task,
    .task_tick          = idle_task_tick,
    .select_task_rq     = NULL,
    .balance            = NULL,
};
