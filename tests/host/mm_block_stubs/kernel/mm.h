/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#pragma once
#include <kernel/kernel.h>
#include <kernel/list.h>
#include <kernel/spinlock.h>
typedef int gfp_t;
struct mem_base { u32 magic; pid_t pid; u32 size; u32 used; struct list_head list; };
struct memblock { addr_t start; size_t size; u32 max_alloc_cap; struct list_head base; spinlock_t lock; struct list_head list; };
addr_t alloc_pages(gfp_t flag, u32 order);
int free_pages(addr_t addr, u32 order);
struct memblock *find_block(void *addr);
struct mem_base *find_base(struct memblock *block, void *addr);
void *krealloc(void *ptr, u32 size, gfp_t flag);

#define MM_TLSF_MAGIC          0x544C5346U
#define MM_TLSF_ALIGN          8U
#define MM_TLSF_MIN_BLOCK_SIZE 16U
#define MM_TLSF_FL_SHIFT       4U
#define MM_TLSF_FL_MAX         24U
#define MM_TLSF_FL_COUNT       (MM_TLSF_FL_MAX - MM_TLSF_FL_SHIFT + 1U)
#define MM_TLSF_SL_LOG2        3U
#define MM_TLSF_SL_COUNT       (1U << MM_TLSF_SL_LOG2)

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

#ifndef GFP_KERNEL
#define GFP_KERNEL 1
#define GFP_ZERO   (1 << 1)
#endif

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

addr_t alloc_page(gfp_t flag);
int free_page(addr_t addr);
int mm_slub_init(void);
struct kmem_cache *kmem_cache_create(const char *name, size_t size, size_t align, u32 flags);
void *kmem_cache_alloc(struct kmem_cache *cache, gfp_t flag);
int kmem_cache_free(struct kmem_cache *cache, void *obj);
void kmem_cache_destroy(struct kmem_cache *cache);
void *mm_slub_kmalloc(size_t size, gfp_t flag);
int mm_slub_kfree(void *ptr);
size_t mm_slub_obj_size(const void *ptr);
