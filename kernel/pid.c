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
        pr_err("PID resource exhaustion\n");
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
    struct pid_range *range, *item;
    struct pid_range *prev = NULL, *next = NULL, *released = NULL;

    if (pid == 0) return 0;
    range = kmalloc(sizeof(*range), GFP_KERNEL);
    if (range == NULL) return -ENOMEM;
    range->min = pid;
    range->max = pid;

    spin_lock(&g_map.lock);
    list_for_each_entry(item, &g_map.list, list) {
        if (pid >= item->min && pid <= item->max) {
            spin_unlock(&g_map.lock);
            kfree(range);
            return -EINVAL;
        }
        if (item->min > pid) {
            next = item;
            break;
        }
        prev = item;
    }
    if (prev != NULL && prev->max != U32_MAX && prev->max + 1 == pid) {
        prev->max = pid;
        if (next != NULL && pid != U32_MAX && pid + 1 == next->min) {
            prev->max = next->max;
            list_del(&next->list);
            released = next;
        }
    } else if (next != NULL && pid != U32_MAX && pid + 1 == next->min) {
        next->min = pid;
    } else {
        list_add_tail(&range->list, next != NULL ? &next->list : &g_map.list);
        range = NULL;
    }
    spin_unlock(&g_map.lock);
    if (released != NULL) kfree(released);
    if (range != NULL) kfree(range);
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
        pr_err("pid_init error\n");
        return;
    }

    range->min = 1;
    range->max = U32_MAX;
    list_add(&range->list, &g_map.list);
}
