/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/mm.h>
#include <kernel/kernel.h>
#include <kernel/printk.h>
#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <kernel/pid.h>
#include <kernel/task.h>
#include <kernel/init.h>
#include <kernel/sleep.h>

extern struct list_head g_node_list;

__init int mm_init(void)
{
    struct mm_node *node;
    int rc;

    list_for_each_entry (node, &g_node_list, list) {
        rc = mm_node_init(node);
        if (rc < 0) {
            pr_err("mm node init, error, rc=%d\r\n", rc);
            continue;
        }
    }
    pr_info("memory: %u/%u\r\n", mm_get_total_page_num(), mm_get_free_page_num());

    return 0;
}

static void mm_deamon_task_entry(void* parameter)
{
    while (1) {
        sleep(1);
        pr_info("%s: test...\r\n", current->name);
    }
}

static int mm_deamon_task_init(void)
{
    struct task_struct *mm_deamon;

    mm_deamon = task_create("mm_deamon", mm_deamon_task_entry, NULL, MM_DEAMON_TASK_PRIO, 10, NULL);
    if (mm_deamon == NULL) {
        pr_fatal("creat mm_deamon task err\n");
        BUG_ON(true);
        return -EINVAL;
    }
    task_ready(mm_deamon);

    return 0;
}
task_init(mm_deamon_task_init);
