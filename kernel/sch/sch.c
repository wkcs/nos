/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[SCH]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/sch.h>
#include <kernel/kernel.h>
#include <kernel/task.h>
#include <kernel/list.h>
#include <kernel/cpu.h>
#include <kernel/spinlock.h>
#include <kernel/init.h>
#include <kernel/printk.h>
#include <string.h>

struct task_struct *g_current_task;

static struct list_head ready_task_list[CONFIG_MAX_PRIORITY];
SPINLOCK(ready_list_lock);
uint32_t scheduler_lock_nest;

u32 g_sys_cycle;
u64 sys_heartbeat_time;
static u32 cpu_total_run_time;
static u32 cpu_total_run_time_save;
static u32 pre_sys_cycle;

static SPINLOCK(g_switch_lock);

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
    g_sys_cycle = 0;
    sys_heartbeat_time = cpu_run_time_us();
    to_task->start_time = sys_heartbeat_time;
    to_task->sys_cycle = g_sys_cycle;
    to_task->save_sys_cycle = 0;
    g_current_task = to_task;
    context_switch_to((addr_t)&to_task->sp);

    /* never come back */
}

static struct task_struct *get_next_task(void)
{
    u32 highest_ready_priority;
    struct task_struct *task;

    spin_lock_irq(&ready_list_lock);
#if CONFIG_MAX_PRIORITY <= 32
    highest_ready_priority = __ffs(ready_task_priority_group) - 1;
#else
    u32 offset;

    offset = __ffs(ready_task_priority_group) - 1;
    highest_ready_priority = (offset << 3) + __ffs(ready_task_table[offset]) - 1;
#endif

    if (list_empty(&ready_task_list[highest_ready_priority])) {
        spin_unlock_irq(&ready_list_lock);
        BUG_ON(true);
        pr_err("prio %u not ready task\r\n", highest_ready_priority);
        return NULL;
    }

    task = list_first_entry(&ready_task_list[highest_ready_priority], struct task_struct, list);
    spin_unlock_irq(&ready_list_lock);

    return task;
}

static void calculate_sys_cycle(void)
{
    static int count = 0;

    count++;
    if (count >= (1000 / CONFIG_SYS_TICK_MS)) {
        count = 0;
        g_sys_cycle++;
    }
}

static void calculate_task_time(struct task_struct *to_task, struct task_struct *from_task)
{
    u64 run_times;
    u32 run_time;

    sys_heartbeat_time = run_times = cpu_run_time_us();
    if (run_times >= from_task->start_time) {
        run_time = run_times - from_task->start_time;
    } else {
        run_time = run_times + U64_MAX - from_task->start_time;
    }
    from_task->run_time += run_time;
    /* 不统计IDEL任务 */
    if (from_task->pid != IDEL_TASK_PID) {
        cpu_total_run_time += run_time;
    }
    to_task->start_time = run_times;
    if (to_task->sys_cycle != g_sys_cycle) {
        to_task->save_sys_cycle = to_task->sys_cycle;
        to_task->sys_cycle = g_sys_cycle;
        to_task->save_run_time = to_task->run_time;
        to_task->run_time = 0;
    }

    if (g_sys_cycle != pre_sys_cycle) {
        cpu_total_run_time_save = cpu_total_run_time;
        cpu_total_run_time = 0;
        pre_sys_cycle = g_sys_cycle;
    }
}

void switch_task(void)
{
    struct task_struct *to_task;
    struct task_struct *from_task;

    if (unlikely(!kernel_running)) {
        return;
    }

    spin_lock_irq(&g_switch_lock);
    from_task = current;
    /* get switch to task */
    to_task = get_next_task();
    /* if the destination task is not the same as current task */
    if (to_task != from_task) {
        spin_lock_irq(&to_task->lock);
        if (to_task->status != TASK_READY && to_task->status != TASK_RUNING) {
            BUG_ON(true);
            pr_err("%s task status=%d\r\n", to_task->name, to_task->status);
        }
        spin_lock_irq(&from_task->lock);
        if (from_task->status == TASK_RUNING) {
            from_task->status = TASK_READY;
        }
        calculate_task_time(to_task, from_task);
        to_task->status = TASK_RUNING;
        spin_unlock_irq(&from_task->lock);
        spin_unlock_irq(&to_task->lock);
        context_switch((addr_t)&from_task->sp, (addr_t)&to_task->sp);
        g_current_task = to_task;
        spin_unlock_irq(&g_switch_lock);
        return;
    } else {
        spin_lock_irq(&to_task->lock);
        to_task->status = TASK_RUNING;
        spin_unlock_irq(&to_task->lock);
    }
    spin_lock_irq(&from_task->lock);
    calculate_task_time(from_task, from_task);
    spin_unlock_irq(&from_task->lock);
    spin_unlock_irq(&g_switch_lock);
}

void add_task_to_ready_list_lock(struct task_struct *task)
{
    if (task == NULL) {
        pr_err("task is NULL\r\n");
        return;
    }

    task->list_lock = &ready_list_lock;
    /* insert task to ready list */
    list_add_tail(&task->list, &(ready_task_list[task->current_priority]));
#if CONFIG_MAX_PRIORITY > 32
    ready_task_table[task->offset] |= task->prio_mask;
#endif
    ready_task_priority_group |= task->offset_mask;
    /* change status */
    task->status = TASK_READY;
    task->remaining_tick = task->init_tick;
}

void add_task_to_ready_list(struct task_struct *task)
{
    if (task == NULL) {
        pr_err("task is NULL\r\n");
        return;
    }

    spin_lock_irq(&task->lock);
    task->list_lock = &ready_list_lock;
    spin_lock_irq(task->list_lock);

    /* insert task to ready list */
    list_add_tail(&task->list, &(ready_task_list[task->current_priority]));

#if CONFIG_MAX_PRIORITY > 32
    ready_task_table[task->offset] |= task->prio_mask;
#endif
    ready_task_priority_group |= task->offset_mask;

    /* change status */
    task->status = TASK_READY;
    task->remaining_tick = task->init_tick;

    spin_unlock_irq(task->list_lock);
    spin_unlock_irq(&task->lock);
}

void del_task_to_ready_list_lock(struct task_struct *task)
{
    if (task == NULL) {
        pr_err("task is NULL\r\n");
        return;
    }

    if ((task->list_lock != &ready_list_lock) ||
        (task->status != TASK_READY && task->status != TASK_RUNING)) {
        pr_err("%s is not ready task, status=%u\r\n", task->name, task->status);
        return;
    }
    if (list_empty(&(task->list))) {
        return;
    }

    /* remove task from ready list */
    list_del(&(task->list));
    if (list_empty(&(ready_task_list[task->current_priority]))) {
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
    task->list_lock = NULL;
    task->status = TASK_SUSPEND;
}

void del_task_to_ready_list(struct task_struct *task)
{
    if (task == NULL) {
        pr_err("task is NULL\r\n");
        return;
    }

    spin_lock_irq(&task->lock);
    if ((task->list_lock != &ready_list_lock) ||
        (task->status != TASK_READY && task->status != TASK_RUNING)) {
        spin_unlock_irq(&task->lock);
        pr_err("%s is not ready task, status=%u\r\n", task->name, task->status);
        return;
    }
    spin_unlock_irq(&task->lock);
    if (list_empty(&(task->list))) {
        return;
    }

    spin_lock_irq(&task->lock);
    spin_lock_irq(task->list_lock);
    /* remove task from ready list */
    list_del(&(task->list));
    if (list_empty(&(ready_task_list[task->current_priority]))) {
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
    spin_unlock_irq(task->list_lock);
    task->status = TASK_SUSPEND;
    task->list_lock = NULL;
    spin_unlock_irq(&task->lock);
}

uint32_t get_sch_lock_level(void)
{
    return scheduler_lock_nest;
}

void sch_heartbeat(void)
{
    struct task_struct *task;
    struct task_struct *next_task;
    bool singular;

    calculate_sys_cycle();
    task = current;
    next_task = get_next_task();
    spin_lock_irq(&task->lock);
    if (likely(task->remaining_tick > 0)) {
        task->remaining_tick--;
    }
    if (unlikely(task != next_task)) {
        /*
         * 优先级最高的任务可能由于switch_pending的限制
         * 并未切换成功，这里检查到如果优先级最高的任务不是
         * 当前任务则需要让出CPU
         */
        task->remaining_tick = 0;
    }
    spin_lock_irq(&ready_list_lock);
    singular = list_is_singular(&ready_task_list[task->current_priority]);
    spin_unlock_irq(&ready_list_lock);
    if ((task->remaining_tick == 0) &&
        (scheduler_lock_nest == 0) && !singular) {
        spin_unlock_irq(&task->lock);
        task_yield_cpu();
        return;
    } else {
        calculate_task_time(task, task);
    }
    spin_unlock_irq(&task->lock);
}

u32 get_cpu_usage(void)
{
    return cpu_total_run_time_save;
}
