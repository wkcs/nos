/**
 * Copyright (C) 2023-2026 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_INTERNAL_SCHED_H__
#define __NOS_INTERNAL_SCHED_H__

#include <kernel/kernel.h>
#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <kernel/sched.h>
#include <kernel/task.h>

#ifndef CONFIG_NR_CPUS
#ifdef CONFIG_SMP
#define CONFIG_NR_CPUS 4
#else
#define CONFIG_NR_CPUS 1
#endif
#endif

#define SCHED_ENQUEUE_WAKEUP  0x01
#define SCHED_ENQUEUE_RESTORE 0x02
#define SCHED_ENQUEUE_MIGRATE 0x04

#define SCHED_DEQUEUE_SLEEP   0x01
#define SCHED_DEQUEUE_MIGRATE 0x04

#define SCHED_LATENCY_US      10000ULL
#define SCHED_MIN_GRAN_US     1000ULL
#define SCHED_WEIGHT_NICE_0   1024U

struct sched_rq;

struct sched_class {
    const char *name;
    const struct sched_class *next;

    void (*enqueue_task)(struct sched_rq *rq, struct task_struct *p, int flags);
    void (*dequeue_task)(struct sched_rq *rq, struct task_struct *p, int flags);
    void (*yield_task)(struct sched_rq *rq, struct task_struct *p);
    void (*check_preempt_curr)(struct sched_rq *rq, struct task_struct *p, int flags);
    struct task_struct *(*pick_next_task)(struct sched_rq *rq);
    void (*put_prev_task)(struct sched_rq *rq, struct task_struct *prev);
    void (*task_tick)(struct sched_rq *rq, struct task_struct *curr);
    uint32_t (*select_task_rq)(struct task_struct *p, uint32_t prev_cpu, int wake_flags);
    int (*balance)(struct sched_rq *this_rq);
};

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
    struct list_head tasks_timeline;
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
    bool                online;

    struct task_struct *curr;
    struct task_struct *idle;

    struct rt_rq        rt;
    struct fair_rq      fair;

    /* Per-CPU PELT & time accounting */
    uint64_t            clock_us;
    uint64_t            busy_time_us;
    uint64_t            idle_time_us;
    uint32_t            win_start_us;
    uint32_t            win_busy_us;
    uint16_t            cpu_util_avg;
};

extern struct sched_rq g_sched_rqs[CONFIG_NR_CPUS];
extern const struct sched_class rt_sched_class;
extern const struct sched_class fair_sched_class;
extern const struct sched_class idle_sched_class;

struct sched_rq *sched_rq_of(uint32_t cpu_id);
struct sched_rq *this_rq(void);

void sched_rt_rq_init(struct rt_rq *rt);
void sched_fair_rq_init(struct fair_rq *fair);
uint16_t sched_nice_to_weight(int8_t nice);
const struct sched_class *sched_policy_to_class(uint8_t policy);

/* PELT & LoadAvg internal accounting (kernel/sched/sched_load.c) */
void sched_load_init(void);
void sched_load_task_init(struct task_struct *p, int8_t nice);
void sched_load_update_task(struct sched_rq *rq, struct task_struct *p,
                            uint64_t now_us, uint64_t delta_us, bool is_idle);
void sched_load_decay_task(struct task_struct *p, uint64_t now_us);
void sched_load_tick(struct sched_rq *rq, uint64_t now_us);
uint16_t sched_load_task_util(struct task_struct *p, uint64_t now_us);
uint16_t sched_load_rq_util(struct sched_rq *rq, uint64_t now_us);

/* Core enqueue/dequeue/migration helpers */
void sched_enqueue_task_locked(struct sched_rq *rq, struct task_struct *p, int flags);
void sched_dequeue_task_locked(struct sched_rq *rq, struct task_struct *p, int flags);
uint32_t sched_select_task_rq(struct task_struct *p, int wake_flags);

#endif /* __NOS_INTERNAL_SCHED_H__ */
