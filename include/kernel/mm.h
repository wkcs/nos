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
    addr_t start;
    size_t size;
    u32 max_alloc_cap;
    struct list_head base;
    spinlock_t lock;
    struct list_head list;
};

struct page;

struct mm_buddy_info {
    struct list_head list;
    spinlock_t lock;
    u32 free_num;
    u32 order;
};

struct mm_buddy {
    struct mm_buddy_info info[CONFIG_MAX_ORDER];
};

struct mm_node {
    const char * const name;
    addr_t start;
    addr_t end;
    u32 start_pfn;
    u32 page_num;
    struct mm_buddy buddy;
    u32 buddy_page_index; /* 可以使用的第一个页 */
    struct list_head list;
};

struct page {
    struct mm_node *node;
    u32 use_cnt;
    s32 private;
    u32 pfn;
    struct list_head list;
};

typedef enum gpf_flag {
    GFP_KERNEL = 1,
    GFP_ZERO = 1 << 1,
} gfp_t;

int mm_node_early_init(void);
__init int mm_node_init(struct mm_node *node);
__init int mm_buddy_init(struct mm_buddy *buddy);
struct mm_node *find_mm_node(addr_t addr);
int mm_init(void);

addr_t alloc_pages(gfp_t flag, u32 order);
addr_t alloc_page(gfp_t flag);
int free_pages(addr_t addr, u32 order);
int free_page(addr_t addr);
void *kalloc(u32 size, gfp_t flag);
void *kalloc_by_pid(u32 size, gfp_t flag, pid_t pid);
int kfree(void *addr);
int kfree_by_pid(pid_t pid);

void mm_buddy_dump_info(struct mm_buddy *buddy);

#define ALIGNED(addr, align) (((addr) + (align) - 1) & ~((align) - 1))
#define ALIGNED_PAGE(addr) ALIGNED(addr, CONFIG_PAGE_SIZE)
#define ALIGNED_DONE(addr, align) ((addr) & ~((align) - 1))
#define ALIGNED_PAGE_DONE(addr) ALIGNED_DONE(addr, CONFIG_PAGE_SIZE)
#define GET_PFN(addr) ((addr) / CONFIG_PAGE_SIZE)

#define mm_node_register(__start, __end, __name) \
__mem struct mm_node mm_node_##__name = { \
    .name = #__name, \
    .start = __start, \
    .end = __end, \
}

#endif /* __NOS_MEM_H__ */
