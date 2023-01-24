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

    return 0;
}
