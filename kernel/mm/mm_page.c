/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/mm.h>
#include <kernel/kernel.h>
#include <kernel/list.h>
#include <kernel/printk.h>

extern struct list_head g_node_list;
extern addr_t __alloc_pages(struct mm_buddy *buddy, gfp_t flag, u32 order);
extern addr_t __alloc_page(struct mm_buddy *buddy, gfp_t flag);
extern int __free_pages(addr_t addr, u32 order);
extern int __free_page(addr_t addr);

addr_t alloc_pages(gfp_t flag, u32 order)
{
    addr_t addr;
    struct mm_node *node;

    list_for_each_entry (node, &g_node_list, list) {
        if (node->type == NODE_RESERVE) {
            continue;
        }
        addr = __alloc_pages(&node->buddy, flag, order);
        if (addr != 0) {
            return addr;
        }
    }

    return addr;
}

addr_t alloc_page(gfp_t flag)
{
    return alloc_pages(flag, 0);
}

int free_pages(addr_t addr, u32 order)
{
    return __free_pages(addr, order);
}

int free_page(addr_t addr)
{
    return __free_page(addr);
}
