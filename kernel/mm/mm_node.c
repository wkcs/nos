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

LIST_HEAD(g_node_list);

extern addr_t __memory_node_data_start;
extern addr_t __memory_node_data_end;

static u32 get_node_num(void)
{
    addr_t addr_size = (addr_t)&__memory_node_data_end - (addr_t)&__memory_node_data_start;

    if (addr_size == 0) {
        return 0;
    }
    if (addr_size % sizeof(struct mm_node) != 0) {
        return 0;
    }

    return (addr_size / sizeof(struct mm_node));
}

static struct mm_node *find_first_node(void)
{
    addr_t start_addr = (addr_t)&__memory_node_data_start;
    return (struct mm_node *)start_addr;
}

static int __mm_node_early_init(struct mm_node *node)
{
    u32 size;
    u32 buddy_page_index;
    u32 page_struct_size;

    if (node->type == NODE_NORMAL) {
        node->start = ALIGNED_PAGE(node->start);
        node->end = ALIGNED_PAGE_DONE(node->end);
        if (node->end < node->start) {
            pr_err("node[%s]: too small(0x%lx-0x%lx), skip1\r\n", node->name, node->start, node->end);
            return -ENOMEM;
        }
        size = node->end - node->start;
        if (size <= sizeof(struct mem_base)) {
            pr_err("node[%s]: too small(0x%lx-0x%lx), skip2\r\n", node->name, node->start, node->end);
            return -ENOMEM;
        }
    } else {
        size = node->end - node->start;
    }
    node->start_pfn = GET_PFN(node->start);
    node->page_num = size / CONFIG_PAGE_SIZE;

    if (node->type == NODE_NORMAL) {
        page_struct_size = node->page_num * sizeof(struct page);
        if (page_struct_size % CONFIG_PAGE_SIZE) {
            buddy_page_index = page_struct_size / CONFIG_PAGE_SIZE + 1;
        } else {
            buddy_page_index = page_struct_size / CONFIG_PAGE_SIZE;
        }
        if (buddy_page_index >= node->page_num) {
            pr_err("node[%s]: too small(0x%lx-0x%lx), skip3\r\n", node->name, node->start, node->end);
            return -ENOMEM;
        }
        node->buddy_page_index = buddy_page_index;
    } else {
        node->buddy_page_index = node->page_num;
    }
    node->free_num = node->page_num - node->buddy_page_index;

    list_add_tail(&node->list, &g_node_list);

    return 0;
}

__init int mm_node_early_init(void)
{
    u32 num, i;
    struct mm_node *node;
    int rc;

    num = get_node_num();
    if (num == 0) {
        pr_fatal("memory node not found\r\n");
        BUG_ON(true);
        return -ENODEV;
    }
    pr_info("find %u memory node\r\n", num);
    node = find_first_node();

    for (i = 0; i < num; i++) {
        rc = __mm_node_early_init(node);
        if (rc == 0) {
            pr_info("node[%s]: 0x%lx-0x%lx, start_pfn=%u, page_num=%u, buddy_page_num=%u\r\n",
                    node->name, node->start, node->end, node->start_pfn, node->page_num,
                    node->page_num - node->buddy_page_index);
        }
        node++;
    }

    return 0;
}

__init int mm_node_init(struct mm_node *node)
{
    struct page *page = (struct page *)node->start;
    u32 i;

    if (node->type == NODE_RESERVE) {
        return 0;
    }

    for (i = 0; i < node->page_num; i++) {
        if (i < node->buddy_page_index) {
            page->use_cnt = 1;
        } else {
            page->use_cnt = 0;
        }
        page->node = node;
        page->private = -1;
        page->pfn = node->start_pfn + i;
        INIT_LIST_HEAD(&page->list);
        page++;
    }

    mm_buddy_init(&node->buddy);
    mm_buddy_dump_info(&node->buddy);

    return 0;
}

struct mm_node *find_mm_node(addr_t addr)
{
    u32 pfn = GET_PFN(addr);
    struct mm_node *node;

    list_for_each_entry (node, &g_node_list, list) {
        if (pfn >= node->start_pfn && pfn < node->start_pfn + node->page_num) {
            return node;
        }
    }

    return NULL;
}

u32 mm_get_free_page_num(void)
{
    u32 num = 0;
    struct mm_node *node;

    list_for_each_entry (node, &g_node_list, list) {
        num += node->free_num;
    }

    return num;
}

u32 mm_get_total_page_num(void)
{
    u32 num = 0;
    struct mm_node *node;

    list_for_each_entry (node, &g_node_list, list) {
        num += node->page_num;
    }

    return num;
}

static void __mm_node_dump(struct mm_node *node)
{
    pr_info("node[%s]: 0x%lx-0x%lx, start_pfn=%u, page_num=%u, buddy_page_num=%u, free_num=%u\r\n",
            node->name, node->start, node->end, node->start_pfn, node->page_num,
            node->page_num - node->buddy_page_index, node->free_num);
    mm_buddy_dump_info(&node->buddy);
}

void mm_node_dump(void)
{
    struct mm_node *node;

    list_for_each_entry (node, &g_node_list, list) {
        __mm_node_dump(node);
    }
}
