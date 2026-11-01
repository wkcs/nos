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
#define MM_TLSF_MAGIC   0x544C5346U

#define MM_TLSF_ALIGN           8U
#define MM_TLSF_MIN_BLOCK_SIZE  16U
#define MM_TLSF_FL_SHIFT        4U
#define MM_TLSF_FL_MAX          24U
#define MM_TLSF_FL_COUNT        (MM_TLSF_FL_MAX - MM_TLSF_FL_SHIFT + 1U)
#define MM_TLSF_SL_LOG2         3U
#define MM_TLSF_SL_COUNT        (1U << MM_TLSF_SL_LOG2)

struct mm_tlsf_block {
    struct mm_tlsf_block *prev_phys;
    size_t size_flags;
    pid_t pid;
    u32 magic;
} __attribute__((aligned(8)));

struct mm_tlsf_pool {
    addr_t start;
    addr_t end;
    size_t total_size;
    size_t used_size;
    u32 fl_bitmap;
    u32 sl_bitmap[MM_TLSF_FL_COUNT];
    struct mm_tlsf_block *blocks[MM_TLSF_FL_COUNT][MM_TLSF_SL_COUNT];
    struct mm_tlsf_block *first_block;
    spinlock_t lock;
    bool initialized;
};

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

enum mm_node_algo {
    MM_NODE_ALGO_AUTO = 0,
    MM_NODE_ALGO_TLSF = 1,
    MM_NODE_ALGO_BUDDY = 2,
};

struct mm_node {
    const char * const name;
    addr_t start;
    addr_t end;
    enum mm_node_type type;
    enum mm_node_algo algo;
    u32 start_pfn;
    u32 page_num;
    u32 free_num;
    u32 buddy_page_index; /* 可以使用的第一个页 */
    struct mm_buddy buddy;
    struct mm_tlsf_pool tlsf;
    struct list_head list;
};

struct page {
    struct mm_node *node;
    u32 use_cnt;
    s32 private;
    u32 pfn;
    struct list_head list;
};

#ifndef __NOS_GFP_T_DEFINED__
#define __NOS_GFP_T_DEFINED__
typedef enum gpf_flag {
    GFP_KERNEL = 1,
    GFP_ZERO = 1 << 1,
} gfp_t;
#endif

int mm_node_early_init(void);
__init int mm_node_init(struct mm_node *node);
__init int mm_buddy_init(struct mm_buddy *buddy);
struct mm_node *find_mm_node(addr_t addr);
int mm_init(void);

addr_t alloc_pages(gfp_t flag, u32 order);
addr_t alloc_page(gfp_t flag);
addr_t alloc_pages_exact(size_t size, gfp_t flag);
int free_pages(addr_t addr, u32 order);
int free_page(addr_t addr);
int free_pages_exact(addr_t addr, size_t size);
void *kmalloc(u32 size, gfp_t flag);
void *kzalloc(u32 size, gfp_t flag);
void *kmalloc_aligned(size_t size, size_t align, gfp_t flag);
void *kzalloc_aligned(size_t size, size_t align, gfp_t flag);
void *krealloc(void *ptr, u32 size, gfp_t flag);
char *kstrdup(const char *s, gfp_t gfp);
void *kalloc_by_pid(u32 size, gfp_t flag, pid_t pid);
int kfree(void *addr);
int kfree_by_pid(pid_t pid);

/* TLSF O(1) low-fragmentation allocator API */
int mm_tlsf_init(struct mm_tlsf_pool *pool, addr_t start, size_t size);
void *mm_tlsf_alloc(struct mm_tlsf_pool *pool, size_t size, size_t align, pid_t pid);
int mm_tlsf_free(struct mm_tlsf_pool *pool, void *ptr);
void *mm_tlsf_realloc(struct mm_tlsf_pool *pool, void *ptr, size_t new_size, pid_t pid);
int mm_tlsf_free_by_pid(struct mm_tlsf_pool *pool, pid_t pid);
size_t mm_tlsf_block_size(const void *ptr);
bool mm_tlsf_owns(const struct mm_tlsf_pool *pool, const void *ptr);
size_t mm_tlsf_free_size(struct mm_tlsf_pool *pool);
size_t mm_tlsf_used_size(struct mm_tlsf_pool *pool);
size_t mm_tlsf_total_size(struct mm_tlsf_pool *pool);
size_t mm_tlsf_max_free_block(struct mm_tlsf_pool *pool);

/* SLUB kmem_cache small-object allocator API */
struct kmem_cache {
    const char *name;
    size_t obj_size;
    size_t slot_size;
    size_t align;
    u32 objs_per_slab;
    u32 flags;
    u32 active_objs;
    u32 total_slabs;
    struct list_head partial;
    struct list_head full;
    spinlock_t lock;
    struct list_head node;
};

int mm_slub_init(void);
struct kmem_cache *kmem_cache_create(const char *name, size_t size, size_t align, u32 flags);
void *kmem_cache_alloc(struct kmem_cache *cache, gfp_t flag);
int kmem_cache_free(struct kmem_cache *cache, void *obj);
void kmem_cache_destroy(struct kmem_cache *cache);
void *mm_slub_kmalloc(size_t size, gfp_t flag);
int mm_slub_kfree(void *ptr);
size_t mm_slub_obj_size(const void *ptr);

void mm_buddy_dump_info(struct mm_buddy *buddy);
u32 mm_get_free_page_num(void);
u32 mm_get_total_page_num(void);
void mm_node_dump(void);
void mm_block_dump(void);
struct memblock *find_block(void *addr);
struct mem_base *find_base(struct memblock *block, void *addr);
size_t mm_block_free_size(void);
size_t mm_block_total_size(void);
size_t mm_block_used_size(void);

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
