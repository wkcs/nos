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

__init int mm_buddy_init(struct mm_buddy *buddy)
{
    struct mm_node *node;
    struct mm_buddy_info *info;
    struct page *page;
    u32 index;
    int i;

    if (buddy == NULL) {
        pr_err("%s: buddy is NULL\n", __func__);
        return -EINVAL;
    }

    node = container_of(buddy, struct mm_node, buddy);
    page = (struct page *)node->start;
    for (i = 0; i < CONFIG_MAX_ORDER; i++) {
        info = &buddy->info[i];
        spin_lock_init(&info->lock);
        INIT_LIST_HEAD(&info->list);
        info->free_num = 0;
        info->order = (u32)i;
    }

    for (index = node->buddy_page_index; index < node->page_num; index++) {
        page[index].private = -1;
        INIT_LIST_HEAD(&page[index].list);
    }

    index = node->buddy_page_index;
    while (index < node->page_num) {
        u32 rel_idx = index - node->buddy_page_index;
        u32 remain = node->page_num - index;
        u32 best_order = 0U;

        for (i = CONFIG_MAX_ORDER - 1; i >= 0; i--) {
            u32 order_pages = 1U << (u32)i;
            if (order_pages <= remain && (rel_idx & (order_pages - 1U)) == 0U) {
                best_order = (u32)i;
                break;
            }
        }

        info = &buddy->info[best_order];
        page[index].private = (int)best_order;
        list_add_tail(&page[index].list, &info->list);
        info->free_num++;
        index += (1U << best_order);
    }

    return 0;
}

static void insert_block_to_buddy(struct mm_buddy_info *info, struct page *head)
{
    struct list_head *tmp;
    struct page *cur;

    head->private = (int)info->order;
    spin_lock_irq(&info->lock);
    tmp = info->list.prev;
    list_for_each_entry (cur, &info->list, list) {
        if (cur->pfn > head->pfn) {
            tmp = cur->list.prev;
            break;
        }
    }
    list_add(&head->list, tmp);
    info->free_num++;
    spin_unlock_irq(&info->lock);
}

static int __mm_buddy_split(struct mm_buddy *buddy, u32 order)
{
    struct mm_buddy_info *info;
    struct mm_buddy_info *last_info;
    struct page *head;
    u32 half;
    int rc;

    if (order >= CONFIG_MAX_ORDER || order == 0) {
        return -ENOMEM;
    }

    info = &buddy->info[order];
    if (info->free_num == 0) {
        rc = __mm_buddy_split(buddy, order + 1);
        if (rc < 0) {
            return rc;
        }
        if (info->free_num == 0) {
            BUG_ON(true);
            return -ENOMEM;
        }
    }

    last_info = &buddy->info[order - 1];
    half = 1U << (order - 1U);

    spin_lock_irq(&info->lock);
    head = list_first_entry(&info->list, struct page, list);
    list_del_init(&head->list);
    head->private = -1;
    info->free_num--;
    spin_unlock_irq(&info->lock);

    insert_block_to_buddy(last_info, head);
    insert_block_to_buddy(last_info, &head[half]);

    return 0;
}

static int mm_buddy_split(struct mm_buddy *buddy, u32 order)
{
    if (buddy == NULL) {
        pr_err("buddy is NULL\n");
        return -EINVAL;
    }

    if (order >= CONFIG_MAX_ORDER || order == 0) {
        return -ENOMEM;
    }

    return __mm_buddy_split(buddy, order);
}

static addr_t mm_buddy_alloc_page(struct mm_buddy *buddy, gfp_t flag, u32 order)
{
    struct mm_buddy_info *info = &buddy->info[order];
    struct page *head;
    u32 num;
    u32 count = 1U << order;

    (void)flag;

    if (info->free_num == 0) {
        return 0;
    }

    spin_lock_irq(&info->lock);
    head = list_first_entry(&info->list, struct page, list);
    list_del_init(&head->list);
    info->free_num--;
    head->node->free_num -= count;
    spin_unlock_irq(&info->lock);

    for (num = 0; num < count; num++) {
        head[num].use_cnt = 1;
        head[num].private = -1;
    }

    return (head->pfn * CONFIG_PAGE_SIZE);
}

addr_t __alloc_pages(struct mm_buddy *buddy, gfp_t flag, u32 order)
{
    struct mm_buddy_info *info;
    int rc;

    if (buddy == NULL) {
        pr_err("%s: buddy is NULL\n", __func__);
        return 0;
    }
    if (order >= CONFIG_MAX_ORDER) {
        return 0;
    }

    info = &buddy->info[order];
    if (info->free_num == 0) {
        rc = mm_buddy_split(buddy, order + 1);
        if (rc < 0) {
            return 0;
        }
        if (info->free_num == 0) {
            BUG_ON(true);
            return 0;
        }
    }

    return mm_buddy_alloc_page(buddy, flag, order);
}

addr_t __alloc_page(struct mm_buddy *buddy, gfp_t flag)
{
    if (buddy == NULL) {
        pr_err("%s: buddy is NULL\n", __func__);
        return 0;
    }
    return __alloc_pages(buddy, flag, 0);
}

int __free_pages(addr_t addr, u32 order)
{
    struct mm_node *node = find_mm_node(addr);
    u32 order_num;
    u32 num;
    u32 idx;
    struct page *page, *pages;
    addr_t pfn;

    if (order >= CONFIG_MAX_ORDER) {
        pr_err("order(=%u) is too big\n", order);
        return -EINVAL;
    }
    order_num = 1U << order;
    if (node == NULL) {
        pr_err("addr(=0x%lx) is not in memory node\n", addr);
        return -EINVAL;
    }
    pages = (struct page *)node->start;
    pfn = GET_PFN(addr);
    idx = (u32)(pfn - node->start_pfn);
    if (idx < node->buddy_page_index || idx + order_num > node->page_num) {
        return -EINVAL;
    }
    page = &pages[idx];

    for (num = 0; num < order_num; num++) {
        page[num].use_cnt = 0;
        page[num].private = -1;
    }
    node->free_num += order_num;
    while (order + 1 < CONFIG_MAX_ORDER) {
        u32 rel_idx = idx - node->buddy_page_index;
        u32 buddy_rel_idx = rel_idx ^ (1U << order);
        u32 buddy_idx = node->buddy_page_index + buddy_rel_idx;
        struct mm_buddy_info *info = &node->buddy.info[order];
        struct page *buddy;
        if (buddy_idx < node->buddy_page_index ||
            buddy_idx + (1U << order) > node->page_num) {
            break;
        }
        buddy = &pages[buddy_idx];
        spin_lock_irq(&info->lock);
        if (buddy->private != (int)order || buddy->use_cnt != 0 || info->free_num == 0) {
            spin_unlock_irq(&info->lock);
            break;
        }
        list_del_init(&buddy->list);
        buddy->private = -1;
        info->free_num--;
        spin_unlock_irq(&info->lock);
        if (buddy_idx < idx) {
            idx = buddy_idx;
        }
        order++;
    }
    insert_block_to_buddy(&node->buddy.info[order], &pages[idx]);

    return 0;
}

addr_t __alloc_pages_exact(struct mm_buddy *buddy, size_t size, gfp_t flag)
{
    u32 needed_pages;
    u32 order = 0U;
    u32 alloc_nr;
    u32 i;
    addr_t addr;

    if (buddy == NULL || size == 0U) {
        return 0U;
    }
    needed_pages = (u32)((size + CONFIG_PAGE_SIZE - 1U) / CONFIG_PAGE_SIZE);
    while (order < CONFIG_MAX_ORDER && (1U << order) < needed_pages) {
        order++;
    }
    if (order >= CONFIG_MAX_ORDER) {
        return 0U;
    }

    addr = __alloc_pages(buddy, flag, order);
    if (addr == 0U) {
        return 0U;
    }

    alloc_nr = 1U << order;
    for (i = needed_pages; i < alloc_nr; i++) {
        (void)__free_pages(addr + (addr_t)i * CONFIG_PAGE_SIZE, 0U);
    }
    return addr;
}

int __free_pages_exact(addr_t addr, size_t size)
{
    u32 needed_pages;
    u32 i;
    int rc = 0;

    if (addr == 0U || size == 0U) {
        return -EINVAL;
    }
    needed_pages = (u32)((size + CONFIG_PAGE_SIZE - 1U) / CONFIG_PAGE_SIZE);
    for (i = 0U; i < needed_pages; i++) {
        int page_rc = __free_pages(addr + (addr_t)i * CONFIG_PAGE_SIZE, 0U);
        if (page_rc < 0) {
            rc = page_rc;
        }
    }
    return rc;
}

int __free_page(addr_t addr)
{
    return __free_pages(addr, 0);
}

void mm_buddy_dump_info(struct mm_buddy *buddy)
{
    struct mm_node *node;
    u32 i, num;

    if (buddy == NULL) {
        pr_err("%s: buddy is NULL\n", __func__);
        return;
    }
    node = container_of(buddy, struct mm_node, buddy);

    num = 0;
    pr_info("%s node buddy info:", node->name);
    for (i = 0; i < CONFIG_MAX_ORDER; i++) {
        pr_info_no_tag("%u->%u, ", i, buddy->info[i].free_num);
        num += buddy->info[i].free_num * (1 << i);
    }
    pr_info_no_tag("page_num=%u\n", num);
}
