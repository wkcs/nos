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

extern struct list_head g_memblock_list;
static SPINLOCK(g_mm_lock);

__init int mm_init(void)
{
    struct memblock *block;
    int rc;

    list_for_each_entry (block, &g_memblock_list, list) {
        rc = memblock_init(block);
        if (rc < 0) {
            pr_err("memblock init, error, rc=%d\r\n", rc);
            continue;
        }
    }

    return 0;
}

void *__kalloc(u32 size, gfp_t flag, pid_t pid)
{
    struct memblock *block;
    struct mem_base *base, *new;
    bool find = false;
    bool recheck = false;
    addr_t addr;
    u32 max = 0;

    spin_lock(&g_mm_lock);
    list_for_each_entry (block, &g_memblock_list, list) {
        if (block->max_alloc_cap >= size) {
            find = true;
            break;
        }
    }
    spin_unlock(&g_mm_lock);
    if (!find) {
        return NULL;
    }
    find = false;

    spin_lock(&block->lock);
    list_for_each_entry (base, &block->base, list) {
#ifdef CONFIG_MM_DEBUG
        if (base->magic != MEM_BASE_MAGIC) {
            spin_unlock(&block->lock);
            BUG_ON(true);
            pr_err("mem_base magic error\r\n");
            return NULL;
        }
#endif /* CONFIG_MM_DEBUG */
        if (!base->used && base->size >= size) {
            find = true;
            base->used = true;
            break;
        }
    }
    spin_unlock(&block->lock);
    if (!find) {
        BUG_ON(true);
        pr_err("mem_base not found\r\n");
        return NULL;
    }

    base->pid = pid;
    if (base->size == block->max_alloc_cap) {
        recheck = true;
    }
    addr = ((addr_t)base) + sizeof(struct mem_base);
    if ((base->size) - size <= sizeof(struct mem_base)) {
        return (void *)addr;
    }

    new = (struct mem_base *)(addr + size);
#ifdef CONFIG_MM_DEBUG
    new->magic = MEM_BASE_MAGIC;
#endif
    new->size = base->size - size - sizeof(struct mem_base);
    base->size = size;
    new->used = false;
    recheck = true;

    spin_lock(&block->lock);
    list_add(&new->list, &base->list);
    if (recheck) {
        list_for_each_entry (base, &block->base, list) {
            if (base->size > max) {
                max = base->size;
            }
        }
        block->max_alloc_cap = max;
    }
    spin_unlock(&block->lock);

    return (void *)addr;
}

static int __kfree_base(struct memblock *block, struct mem_base *base)
{
    struct mem_base *new;

    spin_lock(&block->lock);
    if (base->list.prev != &block->base) {
        new = list_prev_entry(base, list);
        if (!new->used) {
            new->size = base->size + sizeof(struct mem_base);
            list_del(&base->list);
#ifdef CONFIG_MM_DEBUG
            base->magic = 0;
#endif
            base = new;
        }
    }
    if (base->list.next != &block->base) {
        new = list_next_entry(base, list);
        if (!new->used) {
            base->size = new->size + sizeof(struct mem_base);
            list_del(&new->list);
#ifdef CONFIG_MM_DEBUG
            new->magic = 0;
#endif
        }
    }
    base->used = false;
    spin_unlock(&block->lock);

    if (block->max_alloc_cap < base->size) {
        block->max_alloc_cap = base->size;
    }

    return 0;
}

static struct memblock *find_block(void *addr)
{
    struct memblock *block;

    spin_lock(&g_mm_lock);
    list_for_each_entry (block, &g_memblock_list, list) {
        if (((addr_t)addr > block->start) && ((addr_t)addr < block->end)) {
            spin_unlock(&g_mm_lock);
            return block;
        }
    }
    spin_unlock(&g_mm_lock);
    return NULL;
}

static struct mem_base *find_base(struct memblock *block, void *addr)
{
    struct mem_base *base;

    spin_lock(&block->lock);
    list_for_each_entry (base, &block->base, list) {
        addr_t base_start = ((addr_t)base) + sizeof(struct mem_base);
        addr_t base_end = base_start + base->size;
        if (base->used && ((addr_t)addr >= base_start) && ((addr_t)addr < base_end)) {
            spin_unlock(&block->lock);
            return base;
        }
    }
    spin_unlock(&block->lock);
    return NULL;
}

static int kfree_base(struct mem_base *base)
{
    struct memblock *block;

    block = find_block(base);
    if (block == NULL) {
        pr_err("no memblock found for 0x%x\r\n", (addr_t)base);
        return -EINVAL;
    }

    return __kfree_base(block, base);
}

int __kfree(void *addr)
{
    struct memblock *block;
    struct mem_base *base;

    block = find_block(addr);
    if (block == NULL) {
        pr_err("no memblock found for 0x%x\r\n", (addr_t)addr);
        return -EINVAL;
    }
    base = find_base(block, addr);
    if (base == NULL) {
        pr_err("no mem_base found for 0x%x in %s memblock\r\n", (addr_t)addr, block->name);
        return -EINVAL;
    }

    return __kfree_base(block, base);
}

int __kfree_by_pid(pid_t pid)
{
    struct memblock *block;
    struct mem_base *base;
    struct list_head *block_next, *base_next;
    int rc;

    if (list_empty(&g_memblock_list)) {
        return -ENODEV;
    }

    block_next = &g_memblock_list;
check_block:
    spin_lock(&g_mm_lock);
    block_next = block_next->next;
    if (block_next == &g_memblock_list) {
        spin_unlock(&g_mm_lock);
        return 0;
    }
    block = list_entry(block_next, struct memblock, list);
    spin_unlock(&g_mm_lock);

    base_next = &block->base;
check_base:
    spin_lock(&block->lock);
    base_next = base_next->next;
    if (base_next == &block->base) {
        spin_unlock(&block->lock);
        goto check_block;
    }
    base = list_entry(base_next, struct mem_base, list);
    spin_unlock(&block->lock);

    rc = __kfree_base(block, base);
    if (rc < 0) {
        pr_err("pid(=%u) free error, rc=%d\r\n", pid, rc);
        return rc;
    }
    goto check_base;
}
