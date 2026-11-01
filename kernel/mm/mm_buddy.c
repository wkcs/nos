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
    addr_t start_addr;
    u32 aligned_size;
    u32 index;
    u32 order_num;
    u32 num;
    int i;

    if (buddy == NULL) {
        pr_err("%s: buddy is NULL\n", __func__);
        return -EINVAL;
    }

    node = container_of(buddy, struct mm_node, buddy);
    page = (struct page *)node->start;
    start_addr = node->start + node->buddy_page_index * CONFIG_PAGE_SIZE;
    for (i = CONFIG_MAX_ORDER - 1; i >= 0; i--) {
        info = &buddy->info[i];
        order_num = 1 << i;
        aligned_size = order_num * CONFIG_PAGE_SIZE;
        index = GET_PFN(ALIGNED(start_addr, aligned_size)) - node->start_pfn;
        spin_lock_init(&info->lock);
        INIT_LIST_HEAD(&info->list);
        info->free_num = 0;
        info->order = i;

        while (index < node->page_num) {
            if (node->page_num - index < order_num) {
                break;
            }
            if (page[index].private >= 0) {
                index += (1 << page[index].private);
                continue;
            }
            page[index].private = i;
            for (num = 0; num < order_num; num++) {
                list_add_tail(&page[index + num].list, &info->list);
            }
            info->free_num++;
            index += num;
        }
    }

    return 0;
}

static void insert_page_to_buddy(struct mm_buddy_info *info, struct list_head *list, u32 blocks)
{
    struct list_head *tmp;
    struct page *old, *new;
    u32 index = 0;

    old = list_first_entry(list, struct page, list);
    list_for_each_entry (new, list, list) {
        new->private = (index % (1U << info->order) == 0) ? (int)info->order : -1;
        index++;
    }
    spin_lock_irq(&info->lock);
    if (info->free_num == 0) {
        tmp = &info->list;
    } else {
        new = list_first_entry(&info->list, struct page, list);
        tmp = info->list.prev;
        list_for_each_entry (new, &info->list, list) {
            if (new->pfn > old->pfn) {
                tmp = new->list.prev;
                break;
            }
        }
    }
    list_splice(list, tmp);
    info->free_num += blocks;
    spin_unlock_irq(&info->lock);
}

static int __mm_buddy_split(struct mm_buddy *buddy, u32 order)
{
    struct mm_buddy_info *info;
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

    struct mm_buddy_info *last_info = &buddy->info[order - 1];
    LIST_HEAD(tmp_list);
    struct page *page = list_first_entry(&info->list, struct page, list);
    page = &page[(1 << order) - 1];

    spin_lock_irq(&info->lock);
    info->free_num--;
    list_cut_position(&tmp_list, &info->list, &page->list);
    spin_unlock_irq(&info->lock);

    insert_page_to_buddy(last_info, &tmp_list, 2);

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
    LIST_HEAD(tmp_list);
    struct page *page = list_first_entry(&info->list, struct page, list);
    page = &page[(1 << order) - 1];

    if (info->free_num == 0) {
        return 0;
    }

    spin_lock_irq(&info->lock);
    info->free_num--;
    page->node->free_num -= (1 << order);
    list_cut_position(&tmp_list, &info->list, &page->list);
    spin_unlock_irq(&info->lock);

    list_for_each_entry (page, &tmp_list, list) {
        page->use_cnt = 1;
        page->private = -1;
    }

    page = list_first_entry(&tmp_list, struct page, list);
    return (page->pfn * CONFIG_PAGE_SIZE);
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
    page = &pages[pfn - node->start_pfn];

    for (num = 0; num < order_num; num++) {
        page[num].use_cnt = 0;
        page[num].private = -1;
    }
    node->free_num += order_num;
    while (order + 1 < CONFIG_MAX_ORDER) {
        addr_t buddy_pfn = pfn ^ ((addr_t)1U << order);
        struct mm_buddy_info *info = &node->buddy.info[order];
        struct page *buddy;
        if (buddy_pfn < node->start_pfn + node->buddy_page_index ||
            buddy_pfn + (1U << order) > node->start_pfn + node->page_num) break;
        buddy = &pages[buddy_pfn - node->start_pfn];
        spin_lock_irq(&info->lock);
        if (buddy->private != (int)order || info->free_num == 0) {
            spin_unlock_irq(&info->lock);
            break;
        }
        for (num = 0; num < (1U << order); num++) {
            list_del_init(&buddy[num].list);
            buddy[num].private = -1;
        }
        info->free_num--;
        spin_unlock_irq(&info->lock);
        if (buddy_pfn < pfn) pfn = buddy_pfn;
        order++;
    }
    page = &pages[pfn - node->start_pfn];
    LIST_HEAD(tmp_list);
    for (num = 0; num < (1U << order); num++) list_add_tail(&page[num].list, &tmp_list);
    insert_page_to_buddy(&node->buddy.info[order], &tmp_list, 1);

    return 0;
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
