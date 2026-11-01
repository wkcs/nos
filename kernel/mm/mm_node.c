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
#include <string.h>

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

    if (node->algo == MM_NODE_ALGO_AUTO) {
#ifdef CONFIG_MM_TLSF
        node->algo = MM_NODE_ALGO_TLSF;
#else
        node->algo = MM_NODE_ALGO_BUDDY;
#endif
    }

    if (node->type == NODE_NORMAL) {
        if (node->algo == MM_NODE_ALGO_TLSF) {
            node->start = ALIGNED(node->start, MM_TLSF_ALIGN);
            node->end = ALIGNED_DONE(node->end, MM_TLSF_ALIGN);
            if (node->end <= node->start ||
                (node->end - node->start) <= 2U * sizeof(struct mm_tlsf_block) + MM_TLSF_MIN_BLOCK_SIZE) {
                pr_err("node[%s]: too small(0x%lx-0x%lx), skip tlsf\n",
                       node->name, node->start, node->end);
                return -ENOMEM;
            }
            size = (u32)(node->end - node->start);
            node->start_pfn = GET_PFN(node->start);
            node->page_num = size / CONFIG_PAGE_SIZE;
            node->buddy_page_index = 0U;
            node->free_num = node->page_num;
            list_add_tail(&node->list, &g_node_list);
            return 0;
        }
        node->start = ALIGNED_PAGE(node->start);
        node->end = ALIGNED_PAGE_DONE(node->end);
        if (node->end < node->start) {
            pr_err("node[%s]: too small(0x%lx-0x%lx), skip1\n", node->name, node->start, node->end);
            return -ENOMEM;
        }
        size = node->end - node->start;
        if (size <= sizeof(struct mem_base)) {
            pr_err("node[%s]: too small(0x%lx-0x%lx), skip2\n", node->name, node->start, node->end);
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
            pr_err("node[%s]: too small(0x%lx-0x%lx), skip3\n", node->name, node->start, node->end);
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
        pr_fatal("memory node not found\n");
        BUG_ON(true);
        return -ENODEV;
    }
    pr_info("find %u memory node\n", num);
    node = find_first_node();

    for (i = 0; i < num; i++) {
        rc = __mm_node_early_init(node);
        if (rc == 0) {
            pr_info("node[%s]: 0x%lx-0x%lx, start_pfn=%u, page_num=%u, buddy_page_num=%u\n",
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
    int rc;

    if (node->type == NODE_RESERVE) {
        return 0;
    }

    if (node->algo == MM_NODE_ALGO_TLSF) {
        rc = mm_tlsf_init(&node->tlsf, node->start, (size_t)(node->end - node->start));
        if (rc < 0) {
            return rc;
        }
        node->page_num = (u32)(mm_tlsf_total_size(&node->tlsf) / CONFIG_PAGE_SIZE);
        node->free_num = (u32)(mm_tlsf_free_size(&node->tlsf) / CONFIG_PAGE_SIZE);
        pr_info("%s node tlsf pool: total=%uB, max_free=%uB\n",
                node->name,
                (u32)mm_tlsf_total_size(&node->tlsf),
                (u32)mm_tlsf_max_free_block(&node->tlsf));
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
    struct mm_node *node;

    list_for_each_entry (node, &g_node_list, list) {
        if (addr >= node->start && addr < node->end) {
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
        if (node->type == NODE_RESERVE) {
            continue;
        }
        if (node->algo == MM_NODE_ALGO_TLSF) {
            num += (u32)(mm_tlsf_free_size(&node->tlsf) / CONFIG_PAGE_SIZE);
        } else {
            num += node->free_num;
        }
    }

    return num;
}

u32 mm_get_total_page_num(void)
{
    u32 num = 0;
    struct mm_node *node;

    list_for_each_entry (node, &g_node_list, list) {
        if (node->type == NODE_RESERVE) {
            continue;
        }
        if (node->algo == MM_NODE_ALGO_TLSF) {
            num += (u32)(mm_tlsf_total_size(&node->tlsf) / CONFIG_PAGE_SIZE);
        } else {
            num += node->page_num;
        }
    }

    return num;
}

static void __mm_node_dump(struct mm_node *node)
{
    if (node->algo == MM_NODE_ALGO_TLSF) {
        pr_info("node[%s]: 0x%lx-0x%lx (TLSF), total=%uB, used=%uB, free=%uB\n",
                node->name, node->start, node->end,
                (u32)mm_tlsf_total_size(&node->tlsf),
                (u32)mm_tlsf_used_size(&node->tlsf),
                (u32)mm_tlsf_free_size(&node->tlsf));
        return;
    }
    pr_info("node[%s]: 0x%lx-0x%lx, start_pfn=%u, page_num=%u, buddy_page_num=%u, free_num=%u\n",
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

extern void *__kalloc(u32 size, gfp_t flag, pid_t pid);
extern int __kfree(void *addr);
extern int __kfree_by_pid(pid_t pid);

void *mm_backend_alloc(size_t size, size_t align, gfp_t flag, pid_t pid)
{
    struct mm_node *node;
    void *ptr = NULL;

    if (size == 0U) {
        return NULL;
    }

    list_for_each_entry (node, &g_node_list, list) {
        if (node->type == NODE_RESERVE || node->algo != MM_NODE_ALGO_TLSF) {
            continue;
        }
        ptr = mm_tlsf_alloc(&node->tlsf, size, align, pid);
        if (ptr != NULL) {
            if (flag & GFP_ZERO) {
                memset(ptr, 0, size);
            }
            return ptr;
        }
    }

#ifdef CONFIG_MM_SLUB
    if (pid == 0 && align <= MM_TLSF_ALIGN && size <= 2048U) {
        ptr = mm_slub_kmalloc(size, flag);
        if (ptr != NULL) {
            return ptr;
        }
    }
#endif

    ptr = __kalloc((u32)size, flag, pid);
    if (ptr != NULL && (flag & GFP_ZERO)) {
        memset(ptr, 0, size);
    }
    return ptr;
}

int mm_backend_free(void *addr)
{
    struct mm_node *node;

    if (addr == NULL) {
        return 0;
    }

    list_for_each_entry (node, &g_node_list, list) {
        if (node->type == NODE_RESERVE || node->algo != MM_NODE_ALGO_TLSF) {
            continue;
        }
        if (mm_tlsf_owns(&node->tlsf, addr)) {
            return mm_tlsf_free(&node->tlsf, addr);
        }
    }

#ifdef CONFIG_MM_SLUB
    if (mm_slub_obj_size(addr) != 0U) {
        return mm_slub_kfree(addr);
    }
#endif

    return __kfree(addr);
}

void *mm_backend_realloc(void *ptr, size_t size, gfp_t flag)
{
    struct mm_node *node;
    size_t old_size;
    void *new_ptr;

    if (size == 0U) {
        if (ptr != NULL) {
            (void)mm_backend_free(ptr);
        }
        return NULL;
    }
    if (ptr == NULL) {
        return mm_backend_alloc(size, MM_TLSF_ALIGN, flag, 0);
    }

    list_for_each_entry (node, &g_node_list, list) {
        if (node->type == NODE_RESERVE || node->algo != MM_NODE_ALGO_TLSF) {
            continue;
        }
        if (mm_tlsf_owns(&node->tlsf, ptr)) {
            return mm_tlsf_realloc(&node->tlsf, ptr, size, 0);
        }
    }

#ifdef CONFIG_MM_SLUB
    old_size = mm_slub_obj_size(ptr);
    if (old_size != 0U) {
        if (size <= old_size) {
            return ptr;
        }
        new_ptr = mm_backend_alloc(size, MM_TLSF_ALIGN, flag, 0);
        if (new_ptr == NULL) {
            return NULL;
        }
        memcpy(new_ptr, ptr, old_size);
        (void)mm_slub_kfree(ptr);
        return new_ptr;
    }
#endif

    return NULL;
}

int mm_backend_free_by_pid(pid_t pid)
{
    struct mm_node *node;
    bool has_tlsf = false;
    int rc;

    list_for_each_entry (node, &g_node_list, list) {
        if (node->type == NODE_RESERVE || node->algo != MM_NODE_ALGO_TLSF) {
            continue;
        }
        has_tlsf = true;
        (void)mm_tlsf_free_by_pid(&node->tlsf, pid);
    }

    rc = __kfree_by_pid(pid);
    return has_tlsf ? 0 : rc;
}

size_t mm_backend_tlsf_total_size(void)
{
    struct mm_node *node;
    size_t total = 0U;

    list_for_each_entry (node, &g_node_list, list) {
        if (node->type == NODE_NORMAL && node->algo == MM_NODE_ALGO_TLSF) {
            total += mm_tlsf_total_size(&node->tlsf);
        }
    }
    return total;
}

size_t mm_backend_tlsf_used_size(void)
{
    struct mm_node *node;
    size_t used = 0U;

    list_for_each_entry (node, &g_node_list, list) {
        if (node->type == NODE_NORMAL && node->algo == MM_NODE_ALGO_TLSF) {
            used += mm_tlsf_used_size(&node->tlsf);
        }
    }
    return used;
}

size_t mm_backend_tlsf_free_size(void)
{
    struct mm_node *node;
    size_t free_bytes = 0U;

    list_for_each_entry (node, &g_node_list, list) {
        if (node->type == NODE_NORMAL && node->algo == MM_NODE_ALGO_TLSF) {
            free_bytes += mm_tlsf_free_size(&node->tlsf);
        }
    }
    return free_bytes;
}
