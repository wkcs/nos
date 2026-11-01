/**
 * Copyright (C) 2023-2026 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <nos/sched.h>

extern u64 cpu_run_time_us(void);

#define LOADAVG_PERIOD_US    5000000ULL
#define EXP_1                1884U
#define EXP_5                2014U
#define EXP_15               2037U
#define PELT_MAX_DECAY_STEPS 8U

static uint32_t g_avenrun[3];
static uint64_t g_last_loadavg_update_us;

__attribute__((weak)) uint32_t task_total_count(void)
{
    return 0U;
}

void sched_load_init(void)
{
    g_avenrun[0] = 0U;
    g_avenrun[1] = 0U;
    g_avenrun[2] = 0U;
    g_last_loadavg_update_us = 0ULL;
}

void sched_load_task_init(struct task_struct *p, int8_t nice)
{
    if (p == NULL) {
        return;
    }

    if (nice < -20) {
        nice = -20;
    } else if (nice > 19) {
        nice = 19;
    }

    INIT_LIST_HEAD(&p->se.run_node);
    p->nice = nice;
    p->se.vruntime = 0ULL;
    p->se.sum_exec_us = 0ULL;
    p->se.exec_start_us = 0ULL;
    p->se.win_start_us = 0U;
    p->se.win_run_us = 0U;
    p->se.util_avg = 0U;
    p->se.weight = sched_nice_to_weight(nice);
    p->se.nvcsw = 0U;
    p->se.nivcsw = 0U;
    p->se.on_rq = false;
}

void sched_load_decay_task(struct task_struct *p, uint64_t now_us)
{
    uint32_t now32;
    uint32_t elapsed;
    uint32_t periods;
    uint32_t steps = 0U;

    if (p == NULL) {
        return;
    }

    now32 = (uint32_t)now_us;
    elapsed = now32 - p->se.win_start_us;
    if (elapsed < SCHED_PELT_WINDOW_US) {
        return;
    }

    periods = elapsed / SCHED_PELT_WINDOW_US;
    while (periods > 0U && steps < PELT_MAX_DECAY_STEPS) {
        uint32_t chunk = (p->se.win_run_us > SCHED_PELT_WINDOW_US) ?
                         SCHED_PELT_WINDOW_US : p->se.win_run_us;
        uint32_t win_util = (uint32_t)(((uint64_t)chunk * SCHED_CAPACITY_SCALE) /
                                       SCHED_PELT_WINDOW_US);

        if (win_util > SCHED_CAPACITY_SCALE) {
            win_util = SCHED_CAPACITY_SCALE;
        }
        if (p->se.util_avg == 0U && win_util > 0U) {
            p->se.util_avg = (uint16_t)win_util;
        } else {
            p->se.util_avg = (uint16_t)(((uint32_t)p->se.util_avg * 3U + win_util) >> 2);
        }
        p->se.win_run_us -= chunk;
        p->se.win_start_us += SCHED_PELT_WINDOW_US;
        periods--;
        steps++;
    }

    if (periods > 0U) {
        p->se.win_run_us = 0U;
        p->se.win_start_us += periods * SCHED_PELT_WINDOW_US;
    }
}

static void sched_load_decay_rq(struct sched_rq *rq, uint64_t now_us)
{
    uint32_t now32;
    uint32_t elapsed;
    uint32_t periods;
    uint32_t steps = 0U;

    if (rq == NULL) {
        return;
    }

    now32 = (uint32_t)now_us;
    elapsed = now32 - rq->win_start_us;
    if (elapsed < SCHED_PELT_WINDOW_US) {
        return;
    }

    periods = elapsed / SCHED_PELT_WINDOW_US;
    while (periods > 0U && steps < PELT_MAX_DECAY_STEPS) {
        uint32_t chunk = (rq->win_busy_us > SCHED_PELT_WINDOW_US) ?
                         SCHED_PELT_WINDOW_US : rq->win_busy_us;
        uint32_t win_util = (uint32_t)(((uint64_t)chunk * SCHED_CAPACITY_SCALE) /
                                       SCHED_PELT_WINDOW_US);

        if (win_util > SCHED_CAPACITY_SCALE) {
            win_util = SCHED_CAPACITY_SCALE;
        }
        if (rq->cpu_util_avg == 0U && win_util > 0U) {
            rq->cpu_util_avg = (uint16_t)win_util;
        } else {
            rq->cpu_util_avg = (uint16_t)(((uint32_t)rq->cpu_util_avg * 3U + win_util) >> 2);
        }
        rq->win_busy_us -= chunk;
        rq->win_start_us += SCHED_PELT_WINDOW_US;
        periods--;
        steps++;
    }

    if (periods > 0U) {
        rq->win_busy_us = 0U;
        rq->win_start_us += periods * SCHED_PELT_WINDOW_US;
    }
}

void sched_load_update_task(struct sched_rq *rq, struct task_struct *p,
                            uint64_t now_us, uint64_t delta_us, bool is_idle)
{
    uint64_t start_us = (now_us >= delta_us) ? (now_us - delta_us) : 0ULL;

    if (rq != NULL) {
        sched_load_decay_rq(rq, start_us);
        rq->clock_us = now_us;
        if (is_idle) {
            rq->idle_time_us += delta_us;
        } else {
            rq->busy_time_us += delta_us;
            rq->win_busy_us += (uint32_t)delta_us;
        }
        sched_load_decay_rq(rq, now_us);
    }

    if (p != NULL) {
        p->se.exec_start_us = now_us;
        p->se.sum_exec_us += delta_us;

        if (!is_idle && p->policy != SCHED_POLICY_IDLE) {
            sched_load_decay_task(p, start_us);
            p->se.win_run_us += (uint32_t)delta_us;

            if (p->policy == SCHED_POLICY_NORMAL) {
                uint32_t weight = (p->se.weight != 0U) ?
                                  p->se.weight : SCHED_WEIGHT_NICE_0;
                p->se.vruntime += (delta_us * (uint64_t)SCHED_WEIGHT_NICE_0) / weight;
            }
            sched_load_decay_task(p, now_us);
        } else {
            p->se.win_start_us = (uint32_t)now_us;
            p->se.win_run_us = 0U;
            p->se.util_avg = 0U;
        }
    }
}

uint16_t sched_load_task_util(struct task_struct *p, uint64_t now_us)
{
    if (p == NULL || p->policy == SCHED_POLICY_IDLE) {
        return 0U;
    }

    if (now_us > 0ULL) {
        sched_load_decay_task(p, now_us);
    }

    if (p->se.util_avg > 0U) {
        return (p->se.util_avg > SCHED_CAPACITY_SCALE) ?
               (uint16_t)SCHED_CAPACITY_SCALE : p->se.util_avg;
    }

    if (p->se.win_run_us > 0U) {
        uint32_t util = (uint32_t)(((uint64_t)p->se.win_run_us * SCHED_CAPACITY_SCALE) /
                                   SCHED_PELT_WINDOW_US);
        return (util > SCHED_CAPACITY_SCALE) ?
               (uint16_t)SCHED_CAPACITY_SCALE : (uint16_t)util;
    }

    return 0U;
}

uint16_t sched_load_rq_util(struct sched_rq *rq, uint64_t now_us)
{
    if (rq == NULL) {
        return 0U;
    }

    if (now_us > 0ULL) {
        sched_load_decay_rq(rq, now_us);
    }

    if (rq->cpu_util_avg > 0U) {
        return (rq->cpu_util_avg > SCHED_CAPACITY_SCALE) ?
               (uint16_t)SCHED_CAPACITY_SCALE : rq->cpu_util_avg;
    }

    if (rq->win_busy_us > 0U) {
        uint32_t util = (uint32_t)(((uint64_t)rq->win_busy_us * SCHED_CAPACITY_SCALE) /
                                   SCHED_PELT_WINDOW_US);
        return (util > SCHED_CAPACITY_SCALE) ?
               (uint16_t)SCHED_CAPACITY_SCALE : (uint16_t)util;
    }

    return 0U;
}

uint16_t sched_get_cpu_util(uint32_t cpu_id)
{
    struct sched_rq *rq = sched_rq_of(cpu_id);
    uint64_t now_us;

    if (rq == NULL) {
        return 0U;
    }
    now_us = (rq->clock_us != 0ULL) ? rq->clock_us : cpu_run_time_us();
    return sched_load_rq_util(rq, now_us);
}

u32 get_cpu_usage(void)
{
    struct sched_rq *rq = this_rq();
    uint64_t now_us = cpu_run_time_us();
    uint16_t util;

    if (now_us == 0ULL && rq != NULL) {
        now_us = rq->clock_us;
    }
    util = sched_load_rq_util(rq, now_us);
    return ((u32)util * 10000U) / SCHED_CAPACITY_SCALE;
}

static uint32_t calc_load(uint32_t load, uint32_t exp, uint32_t active)
{
    uint64_t newload = (uint64_t)load * exp +
                       (uint64_t)active * (SCHED_FIXED_1 - exp);
    if (active >= load) {
        newload += SCHED_FIXED_1 - 1U;
    }
    return (uint32_t)(newload >> SCHED_FIXED_SHIFT);
}

void sched_load_tick(struct sched_rq *rq, uint64_t now_us)
{
    uint32_t active_fixed = 0U;
    uint64_t elapsed;
    uint32_t steps;
    uint32_t i;

    if (rq != NULL && rq->cpu_id != 0U) {
        return;
    }
    if (now_us < g_last_loadavg_update_us + LOADAVG_PERIOD_US) {
        return;
    }

    for (i = 0U; i < CONFIG_NR_CPUS; i++) {
        struct sched_rq *cpu_rq = &g_sched_rqs[i];

        if (cpu_rq->online || i == 0U) {
            uint32_t run_fixed = cpu_rq->nr_running * SCHED_FIXED_1;
            uint32_t util_fixed = ((uint32_t)cpu_rq->cpu_util_avg * SCHED_FIXED_1) >>
                                  SCHED_CAPACITY_SHIFT;
            if (util_fixed > run_fixed) {
                run_fixed = util_fixed;
            }
            active_fixed += run_fixed + cpu_rq->nr_uninterruptible * SCHED_FIXED_1;
        }
    }

    elapsed = now_us - g_last_loadavg_update_us;
    steps = (uint32_t)(elapsed / LOADAVG_PERIOD_US);
    if (steps > 12U) {
        steps = 12U;
    }

    for (i = 0U; i < steps; i++) {
        g_avenrun[0] = calc_load(g_avenrun[0], EXP_1, active_fixed);
        g_avenrun[1] = calc_load(g_avenrun[1], EXP_5, active_fixed);
        g_avenrun[2] = calc_load(g_avenrun[2], EXP_15, active_fixed);
    }
    g_last_loadavg_update_us = now_us;
}

void sched_get_loadavg(uint32_t loads[3], uint32_t *nr_running, uint32_t *nr_total)
{
    uint32_t run_cnt = 0U;
    uint32_t total_cnt = 0U;
    uint32_t i;

    if (loads != NULL) {
        loads[0] = g_avenrun[0];
        loads[1] = g_avenrun[1];
        loads[2] = g_avenrun[2];
    }

    for (i = 0U; i < CONFIG_NR_CPUS; i++) {
        if (g_sched_rqs[i].online || i == 0U) {
            run_cnt += g_sched_rqs[i].nr_running;
            total_cnt += g_sched_rqs[i].nr_running +
                         g_sched_rqs[i].nr_uninterruptible +
                         ((g_sched_rqs[i].idle != NULL) ? 1U : 0U);
        }
    }

    if (nr_running != NULL) {
        *nr_running = run_cnt;
    }
    if (nr_total != NULL) {
        uint32_t all_tasks = task_total_count();
        if (all_tasks > total_cnt) {
            total_cnt = all_tasks;
        }
        *nr_total = total_cnt;
    }
}
