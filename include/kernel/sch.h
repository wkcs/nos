/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_SCH_H__
#define __NOS_SCH_H__

#include <kernel/kernel.h>

struct task_struct;

void sch_init(void);
void sch_start(void);
void switch_task(void);
void add_task_to_ready_list_lock(struct task_struct *task);
void add_task_to_ready_list(struct task_struct *task);
void del_task_to_ready_list_lock(struct task_struct *task);
void del_task_to_ready_list(struct task_struct *task);
uint32_t get_sch_lock_level(void);
void sch_heartbeat(void);
u32 get_cpu_usage(void);

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

#endif /* __NOS_SCH_H__ */
