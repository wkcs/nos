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

LIST_HEAD(g_memblock_list);

extern addr_t __memory_block_data_start;
extern addr_t __memory_block_data_end;

static u32 get_memblock_num(void)
{
    addr_t addr_size = (addr_t)&__memory_block_data_end - (addr_t)&__memory_block_data_start;

    if (addr_size == 0) {
        return 0;
    }
    if (addr_size % sizeof(struct memblock) != 0) {
        return 0;
    }

    return (addr_size / sizeof(struct memblock));
}

static struct memblock *find_first_memblock(void)
{
    addr_t start_addr = (addr_t)&__memory_block_data_start;
    return (struct memblock *)start_addr;
}

__init int memblock_early_init(void)
{
    u32 num, i;
    u32 size;
    struct memblock *block;

    num = get_memblock_num();
    if (num == 0) {
        pr_fatal("memblock not found\r\n");
        return -ENODEV;
    }
    pr_info("find %u memblock\r\n", num);
    block = find_first_memblock();

    for (i = 0; i < num; i++) {
        size = block->end - block->start;
        if (size <= sizeof(struct mem_base)) {
            pr_err("memblock[0x%x - 0x%x] too small, skip\r\n", block->start, block->end);
            continue;
        }
        list_add_tail(&block->list, &g_memblock_list);
        INIT_LIST_HEAD(&block->base);
        spin_lock_init(&block->lock);
        pr_info("memblock[%s]: 0x%x - 0x%x\r\n", block->name, block->start, block->end);
        block++;
    }

    return 0;
}

__init int memblock_init(struct memblock *block)
{
    u32 size;
    struct mem_base *base;

    if (block == NULL) {
        pr_err("memory block is NULL\r\n");
        return -EINVAL;
    }

    size = block->end - block->start - sizeof(struct mem_base);
    block->max_alloc_cap = size;

    base = (struct mem_base *)(block->start);
#ifdef CONFIG_MM_DEBUG
    base->magic = MEM_BASE_MAGIC;
#endif
    base->used = 0;
    base->size = size;
    list_add(&base->list, &block->base);

    return 0;
}
