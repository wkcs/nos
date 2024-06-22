/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[TASK]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/task.h>
#include <kernel/sch.h>
#include <string.h>
#include <kernel/irq.h>
#include <kernel/cpu.h>
#include <kernel/mm.h>
#include <kernel/list.h>
#include <kernel/pid.h>
#include <kernel/spinlock.h>
#include <kernel/printk.h>
#include <kernel/timer.h>

static LIST_HEAD(g_task_list);
LIST_HEAD(g_close_task_list);
static SPINLOCK(g_task_list_lock);
static SPINLOCK(g_close_list_lock);

extern spinlock_t ready_list_lock;

static void task_exit(void)
{
    struct task_struct *task;

    task = current;
    task_del(task);
}

static void timeout(void *parameter)
{
    struct task_struct *task;

    task = (struct task_struct *)parameter;

    BUG_ON(task == NULL);
    BUG_ON(task->status != TASK_WAIT);

    spin_lock_irq(&task->lock);
    task->flag = -ETIMEDOUT;
    if (task->list_lock) {
        spin_lock_irq(task->list_lock);
        list_del(&task->list);
        spin_unlock_irq(task->list_lock);
        task->list_lock = NULL;
    }
    spin_unlock_irq(&task->lock);
    add_task_to_ready_list(task);
}

static void *alloc_task_stack(uint32_t stack_size)
{
    char *buf;
    uint32_t i;

    buf = kmalloc(stack_size, GFP_KERNEL);
    if (buf == NULL) {
        pr_err("alloc stack error\r\n");
        return NULL;
    }

    for (i = 0; i < stack_size; i++) {
        buf[i] = '#';
    }

    return buf;
}

static int free_task_stack(void *stack_addr)
{
    return kfree(stack_addr);
}

static int __task_create(struct task_struct *task,
                         const char *name,
                         void (*entry)(void *parameter),
                         void *parameter,
                         addr_t *stack_start,
                         uint8_t priority,
                         uint32_t tick,
                         void (*clean)(struct task_struct *task))
{
    int rc;

    INIT_LIST_HEAD(&task->list);
    INIT_LIST_HEAD(&task->tlist);
    spin_lock_init(&task->lock);

    task->list_lock = NULL;
    task->name = name;
    task->entry = (void *)entry;
    task->parameter = parameter;
    task->stack = stack_start;
    task->sp = (addr_t *)stack_init(task->entry, task->parameter,
                                    (addr_t *)task->stack,
                                    (void *)task_exit);
    task->init_priority    = priority;
    task->current_priority = priority;
#if CONFIG_MAX_PRIORITY > 32
    task->offset = priority >> 3;
    task->offset_mask = 1UL << task->offset;
    task->prio_mask = 1UL << (priority & 0x07);
#else
    task->offset_mask = 1UL << priority;
#endif
    task->init_tick      = tick;
    task->remaining_tick = tick;
    task->status  = TASK_SUSPEND;
    task->cleanup = clean;
    task->flag = 0;
    task->start_time = 0;
    task->run_time = 0;
    task->sys_cycle = 0;
    task->save_sys_cycle = 0;
    task->save_run_time = 0;

    spin_lock(&g_task_list_lock);
    list_add_tail(&task->tlist, &g_task_list);
    spin_unlock(&g_task_list_lock);
    rc = timer_init(&task->timer, task->name, timeout, task);
    if (rc < 0) {
        pr_err("%s init timer error, rc=%d\r\n", task->name, rc);
    }

    return rc;
}

struct task_struct *task_create(const char *name,
                                void (*entry)(void *parameter),
                                void *parameter,
                                uint8_t priority,
                                uint32_t stack_size,
                                uint32_t tick,
                                void (*clean)(struct task_struct *task))
{
    struct task_struct *task;
    addr_t *stack_start;
    pid_t pid;
    void *stack_addr;
    int rc;

#if CONFIG_MAX_PRIORITY < 256
    if (priority >= CONFIG_MAX_PRIORITY) {
        pr_err("%s: Priority should be less than %d\r\n", name, CONFIG_MAX_PRIORITY);
        return NULL;
    }
#endif

    task = kmalloc(sizeof(struct task_struct), GFP_KERNEL);
    if (task == NULL) {
        pr_err("%s: alloc task struct buf error\r\n", name);
        goto task_struct_err;
    }

    task->stack_size = stack_size;
    stack_addr = alloc_task_stack(stack_size);
    if (stack_addr == NULL) {
        pr_err("%s: alloc task stack buf error\r\n", name);
        goto task_stack_err;
    }
#ifdef CONFIG_STACK_GROWSUP
    stack_start = (addr_t *)(stack_addr);
#else
    stack_start = (addr_t *)((addr_t)stack_addr + stack_size - sizeof(addr_t));
#endif

    pid = pid_alloc();
    if (!pid) {
        pr_err("%s: alloc pid error\r\n", name);
        goto pid_err;
    }

    task->pid = pid;

    rc = __task_create(task, name, entry, parameter, stack_start, priority, tick, clean);
    if (rc < 0) {
        pr_err("%s: task_create error, rc=%d\r\n", name, rc);
        goto task_create_err;
    }

    return task;

task_create_err:
    pid_free(pid);
pid_err:
    free_task_stack(stack_addr);
task_stack_err:
    kfree(task);
task_struct_err:
    return NULL;
}

int task_ready(struct task_struct *task)
{
    if (!task) {
        pr_err("task struct is NULL\r\n");
        return -EINVAL;
    }

    if (task->status == TASK_READY || task->status == TASK_RUNING) {
        pr_warning("%s:task is already in a ready state\r\n", task->name);
        return -EINVAL;
    }

    add_task_to_ready_list(task);
    switch_task();

    return 0;
}

int task_yield_cpu(void)
{
    struct task_struct *task;
    spinlock_t *list_lock;

    task = current;
    list_lock = task->list_lock;
    spin_lock_irq(&task->lock);
    if (task->status == TASK_RUNING) {
        task_list_lock(spin_lock_irq, list_lock);
        del_task_to_ready_list_lock(task);
        add_task_to_ready_list_lock(task);
        task_list_unlock(spin_unlock_irq, list_lock);
    }
    switch_task();
    spin_unlock_irq(&task->lock);

    return 0;
}

void task_del(struct task_struct *task)
{
    spinlock_t *list_lock;

    if (!task) {
        pr_err("task struct is NULL\r\n");
        return;
    }

    pr_info("del %s task\r\n", task->name);
    list_lock = task->list_lock;
    spin_lock_irq(&task->lock);
    task_list_lock(spin_lock_irq, list_lock);
    if (task->status == TASK_READY || task->status == TASK_RUNING) {
        del_task_to_ready_list_lock(task);
    }
    task_list_unlock(spin_unlock_irq, list_lock);

    spin_lock(&g_task_list_lock);
    list_del(&task->tlist);
    spin_unlock(&g_task_list_lock);
    task->status = TASK_CLOSE;
    timer_stop(&task->timer);
    task->list_lock = &g_close_list_lock;
    spin_lock_irq(task->list_lock);
    list_add_tail(&task->list, &g_close_task_list);
    spin_unlock_irq(task->list_lock);
    spin_unlock_irq(&task->lock);

    if (task == current) {
        switch_task();
    }
}

int task_hang(struct task_struct *task)
{
    spinlock_t *list_lock;

    if (!task) {
        pr_err("task struct is NULL\r\n");
        return -EINVAL;
    }
    if (task->status != TASK_READY && task->status != TASK_RUNING) {
        pr_err("%s task status is %d, not is TASK_READY or TASK_RUNING\r\n", task->name, task->status);
        BUG_ON(task == current);
        return -EINVAL;
    }

    timer_stop(&task->timer);
    list_lock = task->list_lock;
    spin_lock_irq(&task->lock);
    task_list_lock(spin_lock_irq, list_lock);
    del_task_to_ready_list_lock(task);
    if (task->status == TASK_SUSPEND) {
        task->status = TASK_WAIT;
    } else {
        pr_err("%s task status is %d\r\n", task->name, task->status);
    }
    task_list_unlock(spin_unlock_irq, list_lock);
    spin_unlock_irq(&task->lock);

    return 0;
}

int task_hang_lock(struct task_struct *task)
{
    spinlock_t *list_lock;

    if (!task) {
        pr_err("task struct is NULL\r\n");
        return -EINVAL;
    }
    if (task->status != TASK_READY && task->status != TASK_RUNING) {
        pr_err("%s task status is %d, not is TASK_READY or TASK_RUNING\r\n", task->name, task->status);
        BUG_ON(task == current);
        return -EINVAL;
    }

    timer_stop(&task->timer);
    list_lock = task->list_lock;
    task_list_lock(spin_lock_irq, list_lock);
    del_task_to_ready_list_lock(task);
    if (task->status == TASK_SUSPEND) {
        task->status = TASK_WAIT;
    } else {
        pr_err("%s task status is %d\r\n", task->name, task->status);
    }
    task_list_unlock(spin_unlock_irq, list_lock);

    return 0;
}

int task_resume(struct task_struct *task)
{
    if (!task) {
        pr_err("task struct is NULL\r\n");
        return -EINVAL;
    }
    if (task->status != TASK_WAIT) {
        pr_err("task(=%s) status(=%d) is not TASK_WAIT\r\n", task->name, task->status);
        BUG_ON(true);
        return -EINVAL;
    }

    timer_stop(&task->timer);
    add_task_to_ready_list(task);

    return 0;
}

int task_sleep(u32 tick)
{
    struct task_struct *task;
    int rc;

    if (!tick) {
        pr_err("sleep tick is 0\r\n");
        return -EINVAL;
    }

    task = current;
    spin_lock_irq(&task->lock);
    rc = task_hang_lock(task);
    if (rc) {
        spin_unlock_irq(&task->lock);
        pr_err("%s task hang error, rc=%d\r\n", task->name, rc);
        return rc;
    }
    timer_start(&task->timer, tick);
    spin_unlock_irq(&task->lock);
    switch_task();

    if (task->flag == -ETIMEDOUT)
        task->flag = 0;

    return 0;
}

int task_set_prio(struct task_struct *task, uint8_t prio)
{
    spinlock_t *list_lock;

    if (task == NULL) {
        pr_err("task is NULL\r\n");
        return -EINVAL;
    }

    if (task->current_priority == prio) {
        return 0;
    }

    list_lock = task->list_lock;
    spin_lock_irq(&task->lock);
    task_list_lock(spin_lock_irq, list_lock);
    if (task->status == TASK_READY || task->status == TASK_RUNING) {
        del_task_to_ready_list_lock(task);
        task->current_priority = prio;
#if CONFIG_MAX_PRIORITY > 32
        task->offset = prio >> 3;
        task->offset_mask = 1UL << task->offset;
        task->prio_mask = 1UL << (prio & 0x07);
#else
        task->offset_mask = 1UL << prio;
#endif
        add_task_to_ready_list_lock(task);
    } else {
        task->current_priority = prio;
#if CONFIG_MAX_PRIORITY > 32
        task->offset = prio >> 3;
        task->offset_mask = 1UL << task->offset;
        task->prio_mask = 1UL << (prio & 0x07);
#else
        task->offset_mask = 1UL << prio;
#endif
    }
    task_list_unlock(spin_unlock_irq, list_lock);
    spin_unlock_irq(&task->lock);

    return 0;
}

u32 task_get_cpu_usage(struct task_struct *task)
{
    if (task == NULL) {
        pr_err("task is NULL\r\n");
        return 0;
    }

    if (task->sys_cycle == (g_sys_cycle - 1)) {
        return task->run_time;
    }
    if (task->save_sys_cycle == (g_sys_cycle - 1)) {
        return task->save_run_time;
    }
    return 0;
}

static bool task_stack_check(struct task_struct *task)
{
#ifndef CONFIG_STACK_GROWSUP
    if (*(char *)((addr_t)task->stack - task->stack_size + 4) != '#')
        return false;
    else
        return true;
#else
    if (*(char *)((addr_t)task->stack + task->stack_size) != '#')
        return false;
    else
        return true;
#endif
}

static size_t task_get_stack_max_used(struct task_struct *task)
{
    size_t used, i;
    addr_t addr;

    if (!task_stack_check(task))
        return 0;
    
    used = task->stack_size;

#ifndef CONFIG_STACK_GROWSUP
    addr = (addr_t)task->stack - task->stack_size + 4;
    for (i = 0; i < task->stack_size; i++) {
        if (*(char *)(addr + i) == '#')
            used--;
        else
            return used;
    }
#elif
    addr = (addr_t)task->stack + task->stack_size;
    for (i = 0; i < task->stack_size; i++) {
        if (*(char *)(addr - i) == '#')
            used--;
        else
            return used;
    }
#endif

    return used;
}

static char *get_task_status_string(struct task_struct *task)
{
    if (task == NULL) {
        pr_err("task is NULL\r\n");
        return NULL;
    }

    switch(task->status) {
        case TASK_RUNING:
            return "RUNING";
        case TASK_READY:
            return "READY";
        case TASK_WAIT:
            return "WAIT";
        case TASK_SUSPEND:
            return "SUSPEND";
        case TASK_CLOSE:
            return "CLOSE";
        default:
            return "UNKNOWN";
    }
}

void dump_all_task(void)
{
    struct task_struct *task_temp;
    size_t max_used;
    //spin_lock(&g_task_list_lock);
    pr_info("----------------------------------------------------------------------------------------\r\n");
    pr_info("|      task      |prio|    pid    |stack size|stack lave|stack max used| flag | status |\r\n");
    //pr_info("----------------------------------------------------------------------------------------\r\n");
    list_for_each_entry(task_temp, &g_task_list, tlist) {
        max_used = task_get_stack_max_used(task_temp);
        pr_info("|%16s|%4u|%11u|%10u|%10lu|%14u|%6d|%8s|%s\r\n", 
            task_temp->name, task_temp->current_priority, task_temp->pid, task_temp->stack_size, 
            task_temp->stack_size - ((addr_t)task_temp->stack - (addr_t)task_temp->sp),
            max_used, task_temp->flag, get_task_status_string(task_temp),
            max_used ? "" : "(task stack overflow)");
    }
    pr_info("----------------------------------------------------------------------------------------\r\n");
    //spin_unlock(&g_task_list_lock);
}

static void task_free(struct task_struct *task)
{
    void *stack_addr;

#ifdef CONFIG_STACK_GROWSUP
    stack_addr = (void *)(task->stack);
#else
    stack_addr = (void *)((addr_t)task->stack + sizeof(addr_t) - task->stack_size);
#endif
    pid_free(task->pid);
    free_task_stack(stack_addr);
    kfree(task);
}

void clean_close_task(void)
{
    struct task_struct *task, *tmp;
    LIST_HEAD(tmp_list);

    spin_lock_irq(&g_close_list_lock);
    list_for_each_entry_safe(task, tmp, &g_close_task_list, list) {
        if (task->status != TASK_CLOSE) {
            pr_fatal("%s task status is not close\r\n", task->name);
            list_del(&task->list);
        } else {
            list_del(&task->list);
            list_add_tail(&task->list, &tmp_list);
        }
    }
    spin_unlock_irq(&g_close_list_lock);

    list_for_each_entry_safe (task, tmp, &tmp_list, list) {
        pr_info("free %s task\r\n", task->name);
        task_free(task);
    }
}
