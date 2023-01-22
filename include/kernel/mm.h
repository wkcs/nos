/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_MEM_H__
#define __NOS_MEM_H__

#include <kernel/kernel.h>
#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <kernel/pid.h>

#ifdef CONFIG_MM_DEBUG
#define MEM_BASE_MAGIC  0x19950304
#endif

struct mem_base {
#ifdef CONFIG_MM_DEBUG
    u32 magic;
#endif
    pid_t pid;
    u32 size;
    u32 used;
    struct list_head list;
};

struct memblock {
    const char * const name;
    addr_t start;
    addr_t end;
    u32 max_alloc_cap;
    struct list_head list;
    struct list_head base;
    spinlock_t lock;
};

typedef enum gpf_flag {
    GFP_KERNEL = 1,
    GFP_ZERO = 1 << 1,
} gfp_t;

int memblock_early_init(void);
int memblock_init(struct memblock *block);
int mm_init(void);

void *kalloc(u32 size, gfp_t flag);
void *kalloc_by_pid(u32 size, gfp_t flag, pid_t pid);
int kfree(void *addr);
int kfree_by_pid(pid_t pid);

#define memblock_register(__start, __end, __name) \
__mem struct memblock memblock_##__name = { \
    .name = #__name, \
    .start = __start, \
    .end = __end, \
}

#endif /* __NOS_MEM_H__ */
