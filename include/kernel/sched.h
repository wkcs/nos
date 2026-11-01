/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_SCHED_H__
#define __NOS_SCHED_H__

#include <kernel/kernel.h>

struct task_struct;
struct sched_rq;
struct sched_class;

enum sched_policy {
    SCHED_POLICY_RR     = 0,
    SCHED_POLICY_FIFO   = 1,
    SCHED_POLICY_NORMAL = 2,
    SCHED_POLICY_IDLE   = 3,
};

#define SCHED_CAPACITY_SHIFT 10U
#define SCHED_CAPACITY_SCALE (1U << SCHED_CAPACITY_SHIFT) /* 1024 */
#define SCHED_PELT_WINDOW_US 100000U                      /* 100 ms */
#define SCHED_FIXED_SHIFT    11U
#define SCHED_FIXED_1        (1U << SCHED_FIXED_SHIFT)    /* 2048 */

#define NOS_IPI_RESCHED      1U
#define NOS_CPU_MASK_ALL     0xFFU

#define SCHED_LOAD_INT(x)    ((uint32_t)((x) >> SCHED_FIXED_SHIFT))
#define SCHED_LOAD_FRAC(x)   SCHED_LOAD_INT((((x) & (SCHED_FIXED_1 - 1U)) * 100U) + (SCHED_FIXED_1 / 2U))

void sch_init(void);
void sch_start(void);
void switch_task(void);
void sched_resched_check(void);
void add_task_to_ready_list_lock(struct task_struct *task);
void add_task_to_ready_list(struct task_struct *task);
void del_task_to_ready_list_lock(struct task_struct *task);
void del_task_to_ready_list(struct task_struct *task);
uint32_t get_sch_lock_level(void);
void sch_heartbeat(void);
u32 get_cpu_usage(void);
uint16_t sched_get_cpu_util(uint32_t cpu_id);
void sched_get_loadavg(uint32_t loads[3], uint32_t *nr_running, uint32_t *nr_total);
uint64_t sched_get_total_switches(void);
int sched_balance_rq(uint32_t this_cpu);
void dump_sched_rq(int (*print_fn)(const char *fmt, ...));

extern u32 g_sys_cycle;
extern u64 sys_heartbeat_time;
extern uint32_t scheduler_lock_nest;

static inline void sch_lock(void)
{
    BUG_ON(scheduler_lock_nest >= U32_MAX);
    scheduler_lock_nest++;
}

static inline void sch_unlock(void)
{
    if (scheduler_lock_nest > 0) {
        scheduler_lock_nest--;
    }
}

#endif /* __NOS_SCHED_H__ */
