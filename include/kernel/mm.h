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
} __attribute__((aligned(sizeof(addr_t))));

struct memblock {
    addr_t start;
    size_t size;
    u32 max_alloc_cap;
    struct list_head base;
    spinlock_t lock;
    struct list_head list;
} __attribute__((aligned(sizeof(addr_t))));

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

enum mm_node_type {
    NODE_RESERVE = 0,
    NODE_NORMAL,
};

struct mm_node {
    const char * const name;
    addr_t start;
    addr_t end;
    enum mm_node_type type;
    u32 start_pfn;
    u32 page_num;
    u32 free_num;
    u32 buddy_page_index; /* 可以使用的第一个页 */
    struct mm_buddy buddy;
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
void *kmalloc(u32 size, gfp_t flag);
void *kzalloc(u32 size, gfp_t flag);
void *krealloc(void *ptr, u32 size, gfp_t flag);
void *kalloc_by_pid(u32 size, gfp_t flag, pid_t pid);
int kfree(void *addr);
int kfree_by_pid(pid_t pid);

void mm_buddy_dump_info(struct mm_buddy *buddy);
u32 mm_get_free_page_num(void);
u32 mm_get_total_page_num(void);
void mm_node_dump(void);
void mm_block_dump(void);
struct memblock *find_block(void *addr);
struct mem_base *find_base(struct memblock *block, void *addr);
size_t mm_block_free_size(void);

#define ALIGNED(addr, align) (((addr) + (align) - 1) & ~((align) - 1))
#define ALIGNED_PAGE(addr) ALIGNED(addr, CONFIG_PAGE_SIZE)
#define ALIGNED_DONE(addr, align) ((addr) & ~((align) - 1))
#define ALIGNED_PAGE_DONE(addr) ALIGNED_DONE(addr, CONFIG_PAGE_SIZE)
#define GET_PFN(addr) ((addr) / CONFIG_PAGE_SIZE)

#define __mm_node_register(__start, __end, __type, __name) \
__mem struct mm_node mm_node_##__name = { \
    .name = #__name, \
    .start = __start, \
    .end = __end, \
    .type = __type, \
}
#define mm_reserve_node_register(__start, __end, __name) \
    __mm_node_register(__start, __end, NODE_RESERVE, __name)
#define mm_node_register(__start, __end, __name) \
    __mm_node_register(__start, __end, NODE_NORMAL, __name)

#endif /* __NOS_MEM_H__ */
