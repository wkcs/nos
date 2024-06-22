/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[TIMER]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/timer.h>
#include <kernel/task.h>
#include <kernel/sch.h>
#include <kernel/mm.h>
#include <kernel/cpu.h>
#include <kernel/printk.h>

static LIST_HEAD(g_sys_timer_list);
static SPINLOCK(g_timer_list_lock);

int timer_init(struct timer *timer, const char *name,
               void (*timeout)(void *parameter), void *parameter)
{
    if (timer == NULL) {
        pr_err("timer is NULL\r\n");
        return -EINVAL;
    }

    INIT_LIST_HEAD(&timer->list);
    timer->name = name;
    timer->timeout_func = timeout;
    timer->parameter = parameter;
    timer->init_tick = 0;
    timer->timeout = false;
    spin_lock_init(&timer->lock);

    return 0;
}

struct timer *timer_creat(const char *name, void (*timeout)(void *parameter),
                          void *parameter)
{
    struct timer *timer;
    int rc;

    timer = kmalloc(sizeof(struct timer), GFP_KERNEL);
    if (timer == NULL) {
        pr_err("alloc %s timer buf error\r\n", name);
        return NULL;
    }
    rc = timer_init(timer, name, timeout, parameter);
    if (rc < 0) {
        pr_err("%s timer init error, rc=%d\r\n", name, rc);
        kfree(timer);
        return NULL;
    }

    return timer;
}

int timer_remove(struct timer *timer)
{
    if (timer == NULL) {
        pr_err("timer is NULL\r\n");
        return -EINVAL;
    }

    spin_lock_irq(&g_timer_list_lock);
    list_del(&timer->list);
    spin_unlock_irq(&g_timer_list_lock);

    return 0;
}

static u32 get_lave_tick(struct timer *timer)
{
    u64 run_tick = cpu_run_ticks();
    u64 lave_tick = 0;

    if (timer->timeout) {
        return 0;
    }

    if (timer->timeout_tick >= run_tick) {
        return timer->timeout_tick - run_tick;
    } else {
        lave_tick = U64_MAX - run_tick + timer->timeout_tick;
        if (lave_tick > U32_MAX)
            return 0;
        else
            return lave_tick;
    }
}

int timer_start(struct timer *timer, u32 tick)
{
    struct timer *timer_temp;
    u64 run_times;
    int rc;

    if (timer == NULL) {
        pr_err("timer is NULL\r\n");
        return -EINVAL;
    }
    if (tick == 0) {
        pr_err("%s timer set tick to 0\r\n", timer->name);
        return -EINVAL;
    }

    rc = timer_remove(timer);
    if (rc < 0) {
        pr_err("remove %s timer error, rc=%d\r\n", timer->name, rc);
        return rc;
    }
    spin_lock_irq(&timer->lock);
    if (U64_MAX - cpu_run_ticks() >= timer->init_tick) {
        timer->timeout_tick = cpu_run_ticks() + timer->init_tick;
    } else {
        timer->timeout_tick = timer->init_tick - (U64_MAX - cpu_run_ticks());
    }
    run_times = cpu_run_time_us();
    if (run_times - sys_heartbeat_time > (CONFIG_SYS_TICK_MS * 500)) {
        timer->init_tick = tick;
    } else {
        timer->init_tick = tick + 1;
    }
    timer->timeout = false;

    spin_lock_irq(&g_timer_list_lock);
    if (list_empty(&g_sys_timer_list))
        list_add_tail(&timer->list, &g_sys_timer_list);
    else {
        list_for_each_entry(timer_temp, &g_sys_timer_list, list) {
            u32 tick_list = get_lave_tick(timer_temp);
            u32 tick_new = get_lave_tick(timer);
            if (tick_list < tick_new) {
                continue;
            } else {
                break;
            }
        }
        list_add_tail(&timer->list, &timer_temp->list);
    }
    spin_unlock_irq(&g_timer_list_lock);
    spin_unlock_irq(&timer->lock);

    return 0;
}

int timer_stop(struct timer *timer)
{
    if (timer == NULL) {
        pr_err("timer is NULL\r\n");
        return -EINVAL;
    }
    return timer_remove(timer);
}

void timer_check_handle(void)
{
    struct timer *timer, *tmp;
    LIST_HEAD(tmp_list);

    /* check timeout */
    spin_lock_irq(&g_timer_list_lock);
    if (!list_empty(&g_sys_timer_list)) {
        list_for_each_entry_safe (timer, tmp, &g_sys_timer_list, list) {
            if (get_lave_tick(timer) == 0) {
                timer->timeout = true;
            } else {
                break;
            }
        }
    }
    spin_unlock_irq(&g_timer_list_lock);

    if (unlikely(READ_ONCE(scheduler_lock_nest) != 0))
        return;

    spin_lock_irq(&g_timer_list_lock);
    if (!list_empty(&g_sys_timer_list)) {
        list_for_each_entry_safe (timer, tmp, &g_sys_timer_list, list) {
            if (timer->timeout) {
                list_del(&timer->list);
                list_add_tail(&timer->list, &tmp_list);
            } else {
                break;
            }
        }
    }
    spin_unlock_irq(&g_timer_list_lock);

    if (!list_empty(&tmp_list)) {
        list_for_each_entry (timer, &tmp_list, list) {
            timer->timeout_func(timer->parameter);
        }
        switch_task();
    }
}

void dump_timer(void)
{
    struct timer *timer;

    list_for_each_entry (timer, &g_sys_timer_list, list) {
        pr_info("timer[%s]: lave_ticks=%u\r\n", timer->name, get_lave_tick(timer));
    }
}
