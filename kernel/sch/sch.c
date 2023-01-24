/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/sch.h>
#include <kernel/kernel.h>
#include <kernel/task.h>
#include <kernel/list.h>
#include <kernel/cpu.h>
#include <kernel/spinlock.h>
#include <kernel/init.h>
#include <kernel/printk.h>
#include <lib/string.h>

static struct list_head ready_task_list[CONFIG_MAX_PRIORITY];
SPINLOCK(ready_list_lock);
static uint32_t scheduler_lock_nest;

#if CONFIG_MAX_PRIORITY > 32
uint32_t ready_task_priority_group;
uint8_t ready_task_table[32];
#else
uint32_t ready_task_priority_group;
#endif

void sch_init(void)
{
    register addr_t offset;

    scheduler_lock_nest = 0;

    for (offset = 0; offset < CONFIG_MAX_PRIORITY; offset ++) {
        INIT_LIST_HEAD(&ready_task_list[offset]);
    }

    ready_task_priority_group = 0;
#if CONFIG_MAX_PRIORITY > 32
    /* initialize ready table */
    memset(ready_task_table, 0, sizeof(ready_task_table));
#endif

    idel_task_init();
}

void sch_start(void)
{
    struct task_struct *to_task;
    u32 highest_ready_priority;

#if CONFIG_MAX_PRIORITY > 32
    u32 offset;

    offset = __ffs(ready_task_priority_group) - 1;
    highest_ready_priority = (offset << 3) + __ffs(ready_task_table[offset]) - 1;
#else
    highest_ready_priority = __ffs(ready_task_priority_group) - 1;
#endif

    /* get switch to task */
    to_task = list_entry(ready_task_list[highest_ready_priority].next, struct task_struct, list);
    to_task->status = TASK_RUNING;

    /* switch to new task */
    context_switch_to((addr_t)&to_task->sp);

    /* never come back */
}

static struct task_struct *get_next_task(void)
{
    u32 highest_ready_priority;
    struct task_struct *task;

    spin_lock(&ready_list_lock);
#if CONFIG_MAX_PRIORITY <= 32
    highest_ready_priority = __ffs(ready_task_priority_group) - 1;
#else
    u32 offset;

    offset = __ffs(ready_task_priority_group) - 1;
    highest_ready_priority = (offset << 3) + __ffs(ready_task_table[offset]) - 1;
#endif

    task = list_entry(ready_task_list[highest_ready_priority].next, struct task_struct, list);
    spin_unlock(&ready_list_lock);

    return task;
}

void switch_task(void)
{
    struct task_struct *to_task;
    struct task_struct *from_task;

    if (!kernel_running || scheduler_lock_nest)
        return;

    /* get switch to task */
    to_task = get_next_task();
    /* if the destination task is not the same as current task */
    if (to_task != current) {
        spin_lock(&to_task->lock);
        to_task->status = TASK_RUNING;
        from_task = current;
        spin_lock(&from_task->lock);
        from_task->status = TASK_READY;
        spin_unlock(&from_task->lock);
        spin_unlock(&to_task->lock);
        context_switch((addr_t)&from_task->sp, (addr_t)&to_task->sp);
    }
}

void add_task_to_ready_list(struct task_struct *task)
{
    if (task == NULL) {
        pr_err("%s: task is NULL\r\n", __func__);
        return;
    }

    spin_lock(&ready_list_lock);
    spin_lock(&task->lock);

    /* change status */
    task->status = TASK_READY;
    task->remaining_tick = task->init_tick;

    /* insert task to ready list */
    list_add_tail(&task->list, &(ready_task_list[task->current_priority]));

#if CONFIG_MAX_PRIORITY > 32
    ready_task_table[task->offset] |= task->prio_mask;
#endif
    ready_task_priority_group |= task->offset_mask;

    spin_unlock(&task->lock);
    spin_unlock(&ready_list_lock);
}

void del_task_to_ready_list(struct task_struct *task)
{
    if (task == NULL) {
        pr_err("%s: task is NULL\r\n", __func__);
        return;
    }

    spin_lock(&ready_list_lock);
    spin_lock(&task->lock);

    /* remove task from ready list */
    list_del(&(task->list));
    if (list_empty(&(ready_task_list[task->current_priority])))
    {
#if CONFIG_MAX_PRIORITY > 32
        ready_task_table[task->offset] &= ~task->prio_mask;
        if (ready_task_table[task->offset] == 0)
        {
            ready_task_priority_group &= ~task->offset_mask;
        }
#else
        ready_task_priority_group &= ~task->offset_mask;
#endif
    }

    spin_unlock(&task->lock);
    spin_unlock(&ready_list_lock);
}

void sch_lock(void)
{
    BUG_ON(scheduler_lock_nest >= U32_MAX);
    scheduler_lock_nest++;
}


void sch_unlock(void)
{
    if (scheduler_lock_nest > 0) {
        scheduler_lock_nest--;
    }
    if (scheduler_lock_nest == 0) {
        switch_task();
    }
}

uint32_t get_sch_lock_level(void)
{
    return scheduler_lock_nest;
}
