/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[MM_BLOCK]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/mm.h>
#include <kernel/kernel.h>
#include <kernel/printk.h>

static LIST_HEAD(g_memblock_list);
static SPINLOCK(g_memblock_lock);

static int memblock_init(struct memblock *block)
{
    u32 size;
    struct mem_base *base;

    if (block == NULL) {
        pr_err("memory block is NULL\r\n");
        return -EINVAL;
    }

    size = block->size - sizeof(struct mem_base);
    block->max_alloc_cap = size;
    INIT_LIST_HEAD(&block->base);
    spin_lock_init(&block->lock);

    base = (struct mem_base *)(block->start);
#ifdef CONFIG_MM_DEBUG
    base->magic = MEM_BASE_MAGIC;
#endif
    base->used = 0;
    base->size = size;
    list_add(&base->list, &block->base);

    return 0;
}

static int size_to_order(u32 size)
{
    u32 num = (size % CONFIG_PAGE_SIZE == 0) ? (size / CONFIG_PAGE_SIZE) : (size / CONFIG_PAGE_SIZE + 1);
    int i;

    for (i = 0; i < CONFIG_MAX_ORDER; i++) {
        if (num <= (u32)(1 << i)) {
            return i;
        }
    }

    return -EINVAL;
}

static int memblock_alloc(u32 size, gfp_t flag)
{
    struct memblock *block;
    addr_t addr;
    int order;
    int rc;

    order = size_to_order(size + sizeof(struct memblock) + sizeof(struct mem_base));
    if (order < 0) {
        return order;
    }
    addr = alloc_pages(flag, (u32)order);
    if (addr == 0) {
        return -ENOMEM;
    }

    block = (struct memblock *)addr;
    block->start = addr + sizeof(struct memblock);
    block->size = (1 << (u32)order) * CONFIG_PAGE_SIZE - sizeof(struct memblock);
    rc = memblock_init(block);
    if (rc < 0) {
        free_pages(addr, (u32)order);
        return rc;
    }

    spin_lock_irq(&g_memblock_lock);
    list_add(&block->list, &g_memblock_list);
    spin_unlock_irq(&g_memblock_lock);

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
    int rc;

    /* 内存必须按照cpu位宽的字节对齐 */
    if (size % sizeof(addr_t))
        size = size + sizeof(addr_t) - (size % sizeof(addr_t));

recheck_memblock:
    spin_lock_irq(&g_memblock_lock);
    list_for_each_entry (block, &g_memblock_list, list) {
        if (block->max_alloc_cap >= size) {
            find = true;
            break;
        }
    }
    spin_unlock_irq(&g_memblock_lock);
    if (!find) {
        rc = memblock_alloc(size, flag);
        if (rc < 0) {
            return NULL;
        }
        goto recheck_memblock;
    }
    find = false;

    spin_lock_irq(&block->lock);
    list_for_each_entry (base, &block->base, list) {
#ifdef CONFIG_MM_DEBUG
        if (base->magic != MEM_BASE_MAGIC) {
            spin_unlock_irq(&block->lock);
            BUG_ON(true);
            pr_err("mem_base magic error, addr=0x%p\r\n", base);
            return NULL;
        }
#endif /* CONFIG_MM_DEBUG */
        if (!base->used && base->size >= size) {
            find = true;
            base->used = true;
            break;
        }
    }
    spin_unlock_irq(&block->lock);
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
        if (!recheck) {
        return (void *)addr;
        } else {
            spin_lock_irq(&block->lock);
            goto check_max_alloc_cap;
        }
    }

    new = (struct mem_base *)(addr + size);
#ifdef CONFIG_MM_DEBUG
    new->magic = MEM_BASE_MAGIC;
#endif
    new->size = base->size - size - sizeof(struct mem_base);
    base->size = size;
    new->used = false;
    recheck = true;

    spin_lock_irq(&block->lock);
    list_add(&new->list, &base->list);
check_max_alloc_cap:
    if (recheck) {
        list_for_each_entry (base, &block->base, list) {
            if (!base->used && base->size > max) {
                max = base->size;
            }
        }
        block->max_alloc_cap = max;
    }
    spin_unlock_irq(&block->lock);

    return (void *)addr;
}

static int __kfree_base(struct memblock *block, struct mem_base *base)
{
    struct mem_base *new;

    spin_lock_irq(&block->lock);
    if (base->list.prev != &block->base) {
        new = list_prev_entry(base, list);
        if (!new->used) {
            new->size += base->size + sizeof(struct mem_base);
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
            base->size += new->size + sizeof(struct mem_base);
            list_del(&new->list);
#ifdef CONFIG_MM_DEBUG
            new->magic = 0;
#endif
        }
    }
    base->used = false;
    spin_unlock_irq(&block->lock);

    if (block->max_alloc_cap < base->size) {
        block->max_alloc_cap = base->size;
    }

    return 0;
}

struct memblock *find_block(void *addr)
{
    struct memblock *block;

    spin_lock_irq(&g_memblock_lock);
    list_for_each_entry (block, &g_memblock_list, list) {
        if (((addr_t)addr > block->start) && ((addr_t)addr < (block->start + block->size))) {
            spin_unlock_irq(&g_memblock_lock);
            return block;
        }
    }
    spin_unlock_irq(&g_memblock_lock);
    return NULL;
}

struct mem_base *find_base(struct memblock *block, void *addr)
{
    struct mem_base *base;

    spin_lock_irq(&block->lock);
    list_for_each_entry (base, &block->base, list) {
        addr_t base_start = ((addr_t)base) + sizeof(struct mem_base);
        addr_t base_end = base_start + base->size;
        if (base->used && ((addr_t)addr >= base_start) && ((addr_t)addr < base_end)) {
            spin_unlock_irq(&block->lock);
            return base;
        }
    }
    spin_unlock_irq(&block->lock);
    return NULL;
}

static int kfree_base(struct mem_base *base)
{
    struct memblock *block;

    block = find_block(base);
    if (block == NULL) {
        pr_err("no memblock found for 0x%lx\r\n", (addr_t)base);
        return -EINVAL;
    }

    return __kfree_base(block, base);
}

int __kfree(void *addr)
{
    struct memblock *block;
    struct mem_base *base;

    if (addr == NULL)
    {
        return 0;
    }

    block = find_block(addr);
    if (block == NULL) {
        pr_err("no memblock found for 0x%lx\r\n", (addr_t)addr);
        return -EINVAL;
    }
    base = find_base(block, addr);
    if (base == NULL) {
        pr_err("no mem_base found for 0x%lx in memblock\r\n", (addr_t)addr);
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
    spin_lock_irq(&g_memblock_lock);
    block_next = block_next->next;
    if (block_next == &g_memblock_list) {
        spin_unlock_irq(&g_memblock_lock);
        return 0;
    }
    block = list_entry(block_next, struct memblock, list);
    spin_unlock_irq(&g_memblock_lock);

    base_next = &block->base;
check_base:
    spin_lock_irq(&block->lock);
    base_next = base_next->next;
    if (base_next == &block->base) {
        spin_unlock_irq(&block->lock);
        goto check_block;
    }
    base = list_entry(base_next, struct mem_base, list);
    spin_unlock_irq(&block->lock);

    rc = __kfree_base(block, base);
    if (rc < 0) {
        pr_err("pid(=%u) free error, rc=%d\r\n", pid, rc);
        return rc;
    }
    goto check_base;
}

void __mm_block_dump(struct memblock *block)
{
    struct mem_base *base;
    int i = 0;

    pr_info("mm_block: start=0x%lx, size=%u, free=%u\r\n", block->start, block->size, block->max_alloc_cap);
    spin_lock_irq(&block->lock);
    list_for_each_entry (base, &block->base, list) {
        pr_info("mm_base[%d]: size=%u, used=%u\r\n", i, base->size, base->used);
        i++;
    }
    spin_unlock_irq(&block->lock);
}

void mm_block_dump(void)
{
    struct memblock *block;

    spin_lock_irq(&g_memblock_lock);
    list_for_each_entry (block, &g_memblock_list, list) {
        __mm_block_dump(block);
    }
    spin_unlock_irq(&g_memblock_lock);
}

size_t mm_block_free_size(void)
{
    struct memblock *block;
    size_t size = 0;

    spin_lock_irq(&g_memblock_lock);
    list_for_each_entry (block, &g_memblock_list, list) {
        size += block->max_alloc_cap;
    }
    spin_unlock_irq(&g_memblock_lock);

    return size;
}
