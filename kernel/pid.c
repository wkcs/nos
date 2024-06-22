/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/pid.h>
#include <kernel/kernel.h>
#include <kernel/list.h>
#include <kernel/mm.h>
#include <kernel/spinlock.h>
#include <kernel/printk.h>

struct pid_range {
    u32 min;
    u32 max;
    struct list_head list;
};

struct pid_map {
    spinlock_t lock;
    struct list_head list;
};

static struct pid_map g_map;

pid_t pid_alloc(void)
{
    struct pid_range *range;
    pid_t pid;
    bool free = false;

    spin_lock(&g_map.lock);
    if (list_empty(&g_map.list)) {
        spin_unlock(&g_map.lock);
        pr_err("PID resource exhaustion\r\n");
        return 0;
    }

    range = list_entry(g_map.list.next, struct pid_range, list);
    pid = range->min;
    if (range->min >= range->max) {
        list_del(&range->list);
        free = true;
    }
    range->min++;
    spin_unlock(&g_map.lock);

    if (free) {
        kfree(range);
    }

    return pid;
}

int pid_free(pid_t pid)
{
    struct pid_range *range, *tmp;
    struct pid_range *prev_range = NULL;
    struct pid_range *next_range = NULL;

    if (pid == 0) {
        return 0 ;
    }

    range = kmalloc(sizeof(struct pid_range), GFP_KERNEL);
    if (range == NULL) {
        pr_err("alloc pid range buf error\r\n");
        return -ENOMEM;
    }
    range->min = pid;
    range->max = pid;

    spin_lock(&g_map.lock);
    if (list_empty(&g_map.list)) {
        list_add(&range->list, &g_map.list);
        spin_unlock(&g_map.lock);
        return 0;
    }

    list_for_each_entry (tmp, &g_map.list, list) {
        if (tmp->max < range->min) {
            prev_range = tmp;
        } else if ((tmp->min > 1) && ((tmp->min - 1) == range->max)) {
            next_range = tmp;
        }
    }

    if (prev_range) {
        list_add(&range->list, &prev_range->list);
        if ((prev_range->max < U32_MAX) && ((prev_range->max + 1) == range->min)) {
            prev_range->max = range->max;
        } else {
            prev_range = NULL;
        }
    } else {
        list_add_tail(&range->list, g_map.list.next);
    }

    if (prev_range && next_range) {
        prev_range->max = next_range->max;
        prev_range = NULL;
    } else if (prev_range) {
        prev_range->max = range->max;
        prev_range = NULL;
        next_range = NULL;
    } else if (next_range) {
        next_range->min = range->min;
        prev_range = NULL;
        next_range = NULL;
    } else {
        prev_range = NULL;
        range = NULL;
        next_range = NULL;
    }
    spin_unlock(&g_map.lock);

    if (prev_range) {
        kfree(prev_range);
    }
    if (range) {
        kfree(range);
    }
    if (next_range) {
        kfree(next_range);
    }

    return 0;
}

void pid_init(void)
{
    struct pid_range *range;

    spin_lock_init(&g_map.lock);
    INIT_LIST_HEAD(&g_map.list);

    range = kmalloc(sizeof(struct pid_range), GFP_KERNEL);
    BUG_ON(range == NULL);
    if (range == NULL) {
        pr_err("pid_init error\r\n");
        return;
    }

    range->min = 1;
    range->max = U32_MAX;
    list_add(&range->list, &g_map.list);
}
