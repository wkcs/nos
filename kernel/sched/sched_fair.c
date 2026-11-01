/**
 * Copyright (C) 2023-2026 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <nos/sched.h>
#include <kernel/cpu.h>

/*
 * Linux-style nice (-20 .. +19) to weight mapping table.
 * Nice 0 corresponds to weight 1024; each step changes CPU share by ~25%.
 */
static const uint32_t sched_prio_to_weight[40] = {
    /* -20 */ 88761U, 71755U, 56483U, 46273U, 36291U,
    /* -15 */ 29154U, 23254U, 18705U, 14949U, 11916U,
    /* -10 */  9548U,  7620U,  6100U,  4904U,  3906U,
    /*  -5 */  3121U,  2501U,  1991U,  1586U,  1277U,
    /*   0 */  1024U,   820U,   655U,   526U,   423U,
    /*   5 */   335U,   272U,   215U,   172U,   137U,
    /*  10 */   110U,    87U,    70U,    56U,    45U,
    /*  15 */    36U,    29U,    23U,    18U,    15U,
};

uint16_t sched_nice_to_weight(int8_t nice)
{
    uint32_t weight;
    int idx;

    if (nice < -20) {
        nice = -20;
    } else if (nice > 19) {
        nice = 19;
    }
    idx = (int)nice + 20;
    weight = sched_prio_to_weight[idx];
    return (weight > 65535U) ? 65535U : (uint16_t)weight;
}

void sched_fair_rq_init(struct fair_rq *fair)
{
    if (fair == NULL) {
        return;
    }
    INIT_LIST_HEAD(&fair->tasks_timeline);
    fair->min_vruntime = 0ULL;
    fair->nr_running = 0U;
    fair->load_weight = 0U;
}

static void fair_insert_timeline(struct fair_rq *fair, struct task_struct *p)
{
    struct task_struct *pos;

    list_for_each_entry(pos, &fair->tasks_timeline, se.run_node) {
        if (p->se.vruntime < pos->se.vruntime) {
            list_add_tail(&p->se.run_node, &pos->se.run_node);
            return;
        }
    }
    list_add_tail(&p->se.run_node, &fair->tasks_timeline);
}

static void fair_update_min_vruntime(struct sched_rq *rq)
{
    struct fair_rq *fair = &rq->fair;
    uint64_t vruntime = fair->min_vruntime;
    bool has_candidate = false;

    if (rq->curr != NULL &&
        rq->curr->policy == SCHED_POLICY_NORMAL &&
        rq->curr->status == TASK_RUNNING) {
        vruntime = rq->curr->se.vruntime;
        has_candidate = true;
    }

    if (!list_empty(&fair->tasks_timeline)) {
        struct task_struct *first = list_first_entry(&fair->tasks_timeline,
                                                     struct task_struct,
                                                     se.run_node);
        if (!has_candidate || first->se.vruntime < vruntime) {
            vruntime = first->se.vruntime;
            has_candidate = true;
        }
    }

    if (has_candidate && vruntime > fair->min_vruntime) {
        fair->min_vruntime = vruntime;
    }
}

static void place_entity(struct fair_rq *fair, struct sched_entity *se, bool initial)
{
    if (initial && se->vruntime == 0ULL) {
        se->vruntime = fair->min_vruntime;
    } else {
        uint64_t floor = (fair->min_vruntime > (SCHED_LATENCY_US / 2ULL)) ?
                         (fair->min_vruntime - (SCHED_LATENCY_US / 2ULL)) : 0ULL;
        if (se->vruntime < floor) {
            se->vruntime = floor;
        }
    }
}

static void fair_enqueue_task(struct sched_rq *rq, struct task_struct *p, int flags)
{
    if (rq == NULL || p == NULL || p->se.on_rq) {
        return;
    }

    if (p->se.weight == 0U) {
        p->se.weight = sched_nice_to_weight(p->nice);
    }

    if ((flags & SCHED_ENQUEUE_WAKEUP) != 0 || p->se.sum_exec_us == 0ULL) {
        place_entity(&rq->fair, &p->se, p->se.sum_exec_us == 0ULL);
    }

    fair_insert_timeline(&rq->fair, p);
    rq->fair.nr_running++;
    rq->nr_running++;
    rq->fair.load_weight += p->se.weight;
    p->se.on_rq = true;
    fair_update_min_vruntime(rq);
}

static void fair_dequeue_task(struct sched_rq *rq, struct task_struct *p, int flags)
{
    (void)flags;
    if (rq == NULL || p == NULL || !p->se.on_rq) {
        return;
    }

    list_del_init(&p->se.run_node);
    if (rq->fair.nr_running > 0U) {
        rq->fair.nr_running--;
    }
    if (rq->nr_running > 0U) {
        rq->nr_running--;
    }
    if (rq->fair.load_weight >= p->se.weight) {
        rq->fair.load_weight -= p->se.weight;
    } else {
        rq->fair.load_weight = 0U;
    }
    p->se.on_rq = false;
    fair_update_min_vruntime(rq);
}

static void fair_yield_task(struct sched_rq *rq, struct task_struct *p)
{
    uint64_t target;

    if (rq == NULL || p == NULL) {
        return;
    }

    target = p->se.vruntime + SCHED_MIN_GRAN_US;
    if (!list_empty(&rq->fair.tasks_timeline)) {
        struct task_struct *last = list_last_entry(&rq->fair.tasks_timeline,
                                                   struct task_struct,
                                                   se.run_node);
        if (last != p && (last->se.vruntime + 1ULL) > target) {
            target = last->se.vruntime + 1ULL;
        }
    }
    p->se.vruntime = target;

    if (p->se.on_rq) {
        list_del_init(&p->se.run_node);
        fair_insert_timeline(&rq->fair, p);
    }
    fair_update_min_vruntime(rq);
    rq->need_resched = true;
}

static void fair_check_preempt_curr(struct sched_rq *rq, struct task_struct *p, int flags)
{
    (void)flags;
    if (rq == NULL || p == NULL) {
        return;
    }

    if (rq->curr == NULL ||
        rq->curr == rq->idle ||
        rq->curr->policy == SCHED_POLICY_IDLE) {
        rq->need_resched = true;
        return;
    }

    if (rq->curr->policy == SCHED_POLICY_NORMAL) {
        if (p->se.vruntime + SCHED_MIN_GRAN_US < rq->curr->se.vruntime) {
            rq->need_resched = true;
        }
    }
}

static struct task_struct *fair_pick_next_task(struct sched_rq *rq)
{
    if (rq == NULL || rq->fair.nr_running == 0U ||
        list_empty(&rq->fair.tasks_timeline)) {
        return NULL;
    }

    return list_first_entry(&rq->fair.tasks_timeline, struct task_struct, se.run_node);
}

static void fair_put_prev_task(struct sched_rq *rq, struct task_struct *prev)
{
    if (rq == NULL || prev == NULL) {
        return;
    }

    if (prev->se.on_rq) {
        list_del_init(&prev->se.run_node);
        fair_insert_timeline(&rq->fair, prev);
    }
    fair_update_min_vruntime(rq);
}

static void fair_task_tick(struct sched_rq *rq, struct task_struct *curr)
{
    uint64_t now_us;

    if (rq == NULL || curr == NULL) {
        return;
    }

    if (curr->se.weight == 0U) {
        curr->se.weight = sched_nice_to_weight(curr->nice);
    }

    now_us = cpu_run_time_us();
    if (now_us > curr->se.exec_start_us) {
        sched_load_update_task(rq, curr, now_us, now_us - curr->se.exec_start_us, false);
    }

    if (curr->se.on_rq) {
        list_del_init(&curr->se.run_node);
        fair_insert_timeline(&rq->fair, curr);
    }
    fair_update_min_vruntime(rq);

    if (!list_empty(&rq->fair.tasks_timeline)) {
        struct task_struct *first = list_first_entry(&rq->fair.tasks_timeline,
                                                     struct task_struct,
                                                     se.run_node);
        if (first != curr &&
            first->se.vruntime + SCHED_MIN_GRAN_US < curr->se.vruntime) {
            rq->need_resched = true;
        }
    }
}

const struct sched_class fair_sched_class = {
    .name               = "fair",
    .next               = &idle_sched_class,
    .enqueue_task       = fair_enqueue_task,
    .dequeue_task       = fair_dequeue_task,
    .yield_task         = fair_yield_task,
    .check_preempt_curr = fair_check_preempt_curr,
    .pick_next_task     = fair_pick_next_task,
    .put_prev_task      = fair_put_prev_task,
    .task_tick          = fair_task_tick,
    .select_task_rq     = NULL,
    .balance            = NULL,
};
