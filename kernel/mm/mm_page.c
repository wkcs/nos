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
#include <string.h>

extern struct list_head g_node_list;
extern addr_t __alloc_pages(struct mm_buddy *buddy, gfp_t flag, u32 order);
extern addr_t __alloc_page(struct mm_buddy *buddy, gfp_t flag);
extern addr_t __alloc_pages_exact(struct mm_buddy *buddy, size_t size, gfp_t flag);
extern int __free_pages(addr_t addr, u32 order);
extern int __free_page(addr_t addr);
extern int __free_pages_exact(addr_t addr, size_t size);
static SPINLOCK(g_page_lock);

addr_t alloc_pages(gfp_t flag, u32 order)
{
    addr_t addr = 0;
    struct mm_node *node;
    size_t bytes;

    if (order >= CONFIG_MAX_ORDER) {
        return 0;
    }
    bytes = (size_t)CONFIG_PAGE_SIZE << order;

    spin_lock_irq(&g_page_lock);
    list_for_each_entry (node, &g_node_list, list) {
        if (node->type == NODE_RESERVE) {
            continue;
        }
        if (node->algo == MM_NODE_ALGO_TLSF) {
            addr = (addr_t)mm_tlsf_alloc(&node->tlsf, bytes, CONFIG_PAGE_SIZE, 0);
        } else {
            addr = __alloc_pages(&node->buddy, flag, order);
        }
        if (addr != 0) {
            break;
        }
    }
    spin_unlock_irq(&g_page_lock);
    if (addr != 0 && (flag & GFP_ZERO)) {
        memset((void *)addr, 0, bytes);
    }
    return addr;
}

addr_t alloc_page(gfp_t flag)
{
    return alloc_pages(flag, 0);
}

addr_t alloc_pages_exact(size_t size, gfp_t flag)
{
    addr_t addr = 0;
    struct mm_node *node;
    size_t aligned_bytes;

    if (size == 0U) {
        return 0;
    }
    aligned_bytes = ALIGNED_PAGE(size);

    spin_lock_irq(&g_page_lock);
    list_for_each_entry (node, &g_node_list, list) {
        if (node->type == NODE_RESERVE) {
            continue;
        }
        if (node->algo == MM_NODE_ALGO_TLSF) {
            addr = (addr_t)mm_tlsf_alloc(&node->tlsf, aligned_bytes, CONFIG_PAGE_SIZE, 0);
        } else {
            addr = __alloc_pages_exact(&node->buddy, aligned_bytes, flag);
        }
        if (addr != 0) {
            break;
        }
    }
    spin_unlock_irq(&g_page_lock);
    if (addr != 0 && (flag & GFP_ZERO)) {
        memset((void *)addr, 0, aligned_bytes);
    }
    return addr;
}

int free_pages(addr_t addr, u32 order)
{
    struct mm_node *node;
    int rc;

    spin_lock_irq(&g_page_lock);
    node = find_mm_node(addr);
    if (node != NULL && node->algo == MM_NODE_ALGO_TLSF) {
        rc = mm_tlsf_free(&node->tlsf, (void *)addr);
    } else {
        rc = __free_pages(addr, order);
    }
    spin_unlock_irq(&g_page_lock);
    return rc;
}

int free_page(addr_t addr)
{
    return free_pages(addr, 0);
}

int free_pages_exact(addr_t addr, size_t size)
{
    struct mm_node *node;
    int rc;

    spin_lock_irq(&g_page_lock);
    node = find_mm_node(addr);
    if (node != NULL && node->algo == MM_NODE_ALGO_TLSF) {
        rc = mm_tlsf_free(&node->tlsf, (void *)addr);
    } else {
        rc = __free_pages_exact(addr, size);
    }
    spin_unlock_irq(&g_page_lock);
    return rc;
}
