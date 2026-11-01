/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * Linux-Style SLUB (kmem_cache) Zero-Object-Header Small Object Allocator
 */

#define pr_fmt(fmt) "[MM_SLUB]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/mm.h>
#include <kernel/kernel.h>
#include <kernel/printk.h>
#include <string.h>

#define MM_SLAB_MAGIC       0x534C5542U /* 'SLUB' */
#define MM_SLUB_MAX_CACHES  24U
#define MM_SLUB_SIZE_COUNT  8U

struct mm_slab_hdr {
    u32 magic;
    u32 inuse;
    struct kmem_cache *cache;
    void *freelist;
    struct list_head list;
};

static struct kmem_cache g_cache_pool[MM_SLUB_MAX_CACHES];
static bool g_cache_Slot_used[MM_SLUB_MAX_CACHES];
static SPINLOCK(g_slub_lock);

static const size_t g_kmalloc_sizes[MM_SLUB_SIZE_COUNT] = {
    16U, 32U, 64U, 128U, 256U, 512U, 1024U, 2048U
};
static const char *const g_kmalloc_names[MM_SLUB_SIZE_COUNT] = {
    "kmalloc-16", "kmalloc-32", "kmalloc-64", "kmalloc-128",
    "kmalloc-256", "kmalloc-512", "kmalloc-1024", "kmalloc-2048"
};
static struct kmem_cache *g_kmalloc_caches[MM_SLUB_SIZE_COUNT];
static bool g_slub_initialized;

static inline size_t slub_align_up(size_t val, size_t align)
{
    return (val + align - 1U) & ~(align - 1U);
}

static inline struct mm_slab_hdr *ptr_to_slab(const void *ptr)
{
    uintptr_t page_base = (uintptr_t)ptr & ~((uintptr_t)CONFIG_PAGE_SIZE - 1U);
    return (struct mm_slab_hdr *)page_base;
}

static struct mm_slab_hdr *slab_alloc_page(struct kmem_cache *cache)
{
    addr_t page_addr = alloc_page(GFP_KERNEL);
    struct mm_slab_hdr *slab;
    uintptr_t first_slot;
    u32 i;

    if (page_addr == 0U) {
        return NULL;
    }

    slab = (struct mm_slab_hdr *)page_addr;
    slab->magic = MM_SLAB_MAGIC;
    slab->inuse = 0U;
    slab->cache = cache;
    INIT_LIST_HEAD(&slab->list);

    first_slot = slub_align_up(page_addr + sizeof(struct mm_slab_hdr), cache->align);
    slab->freelist = (void *)first_slot;
    for (i = 0U; i < cache->objs_per_slab; i++) {
        void **slot = (void **)(first_slot + (uintptr_t)i * cache->slot_size);
        if (i + 1U < cache->objs_per_slab) {
            *slot = (void *)(first_slot + (uintptr_t)(i + 1U) * cache->slot_size);
        } else {
            *slot = NULL;
        }
    }
    return slab;
}

struct kmem_cache *kmem_cache_create(const char *name, size_t size, size_t align, u32 flags)
{
    struct kmem_cache *cache = NULL;
    size_t eff_align;
    size_t slot_size;
    size_t hdr_space;
    u32 i;

    if (size == 0U || size > CONFIG_PAGE_SIZE / 2U) {
        return NULL;
    }
    eff_align = (align < sizeof(void *)) ? sizeof(void *) : align;
    if ((eff_align & (eff_align - 1U)) != 0U) {
        return NULL;
    }
    slot_size = slub_align_up(size < sizeof(void *) ? sizeof(void *) : size, eff_align);
    hdr_space = slub_align_up(sizeof(struct mm_slab_hdr), eff_align);
    if (hdr_space + slot_size > CONFIG_PAGE_SIZE) {
        return NULL;
    }

    spin_lock_irq(&g_slub_lock);
    for (i = 0U; i < MM_SLUB_MAX_CACHES; i++) {
        if (!g_cache_Slot_used[i]) {
            g_cache_Slot_used[i] = true;
            cache = &g_cache_pool[i];
            break;
        }
    }
    spin_unlock_irq(&g_slub_lock);

    if (cache == NULL) {
        return NULL;
    }

    memset(cache, 0, sizeof(*cache));
    cache->name = name ? name : "kmem_cache";
    cache->obj_size = size;
    cache->slot_size = slot_size;
    cache->align = eff_align;
    cache->objs_per_slab = (u32)((CONFIG_PAGE_SIZE - hdr_space) / slot_size);
    cache->flags = flags;
    INIT_LIST_HEAD(&cache->partial);
    INIT_LIST_HEAD(&cache->full);
    INIT_LIST_HEAD(&cache->node);
    spin_lock_init(&cache->lock);
    return cache;
}

void *kmem_cache_alloc(struct kmem_cache *cache, gfp_t flag)
{
    struct mm_slab_hdr *slab;
    void *obj;

    if (cache == NULL) {
        return NULL;
    }

    spin_lock_irq(&cache->lock);
    if (list_empty(&cache->partial)) {
        spin_unlock_irq(&cache->lock);
        slab = slab_alloc_page(cache);
        if (slab == NULL) {
            return NULL;
        }
        spin_lock_irq(&cache->lock);
        list_add(&slab->list, &cache->partial);
        cache->total_slabs++;
    }

    slab = list_first_entry(&cache->partial, struct mm_slab_hdr, list);
    obj = slab->freelist;
    slab->freelist = *(void **)obj;
    slab->inuse++;
    cache->active_objs++;
    if (slab->freelist == NULL) {
        list_del(&slab->list);
        list_add(&slab->list, &cache->full);
    }
    spin_unlock_irq(&cache->lock);

    if (flag & GFP_ZERO) {
        memset(obj, 0, cache->obj_size);
    }
    return obj;
}

int kmem_cache_free(struct kmem_cache *cache, void *obj)
{
    struct mm_slab_hdr *slab;
    bool was_full;
    addr_t page_to_free = 0U;

    if (cache == NULL || obj == NULL) {
        return -EINVAL;
    }

    slab = ptr_to_slab(obj);
    if (slab->magic != MM_SLAB_MAGIC || slab->cache != cache) {
        return -EINVAL;
    }

    spin_lock_irq(&cache->lock);
    if (slab->inuse == 0U) {
        spin_unlock_irq(&cache->lock);
        return -EINVAL;
    }
    was_full = (slab->freelist == NULL);
    *(void **)obj = slab->freelist;
    slab->freelist = obj;
    slab->inuse--;
    cache->active_objs--;

    if (slab->inuse == 0U) {
        list_del(&slab->list);
        slab->magic = 0U;
        cache->total_slabs--;
        page_to_free = (addr_t)slab;
    } else if (was_full) {
        list_del(&slab->list);
        list_add(&slab->list, &cache->partial);
    }
    spin_unlock_irq(&cache->lock);

    if (page_to_free != 0U) {
        (void)free_page(page_to_free);
    }
    return 0;
}

void kmem_cache_destroy(struct kmem_cache *cache)
{
    struct list_head *cur, *next;
    u32 idx;

    if (cache == NULL) {
        return;
    }

    spin_lock_irq(&cache->lock);
    cur = cache->partial.next;
    while (cur != &cache->partial) {
        struct mm_slab_hdr *slab = list_entry(cur, struct mm_slab_hdr, list);
        next = cur->next;
        list_del(&slab->list);
        slab->magic = 0U;
        (void)free_page((addr_t)slab);
        cur = next;
    }
    cur = cache->full.next;
    while (cur != &cache->full) {
        struct mm_slab_hdr *slab = list_entry(cur, struct mm_slab_hdr, list);
        next = cur->next;
        list_del(&slab->list);
        slab->magic = 0U;
        (void)free_page((addr_t)slab);
        cur = next;
    }
    spin_unlock_irq(&cache->lock);

    spin_lock_irq(&g_slub_lock);
    idx = (u32)(cache - g_cache_pool);
    if (idx < MM_SLUB_MAX_CACHES) {
        g_cache_Slot_used[idx] = false;
    }
    spin_unlock_irq(&g_slub_lock);
}

int mm_slub_init(void)
{
    u32 i;

    if (g_slub_initialized) {
        return 0;
    }
    for (i = 0U; i < MM_SLUB_SIZE_COUNT; i++) {
        g_kmalloc_caches[i] = kmem_cache_create(
            g_kmalloc_names[i], g_kmalloc_sizes[i], MM_TLSF_ALIGN, 0U);
        if (g_kmalloc_caches[i] == NULL) {
            return -ENOMEM;
        }
    }
    g_slub_initialized = true;
    return 0;
}

void *mm_slub_kmalloc(size_t size, gfp_t flag)
{
    u32 i;

    if (size == 0U || size > g_kmalloc_sizes[MM_SLUB_SIZE_COUNT - 1U]) {
        return NULL;
    }
    if (!g_slub_initialized && mm_slub_init() < 0) {
        return NULL;
    }
    for (i = 0U; i < MM_SLUB_SIZE_COUNT; i++) {
        if (size <= g_kmalloc_sizes[i]) {
            return kmem_cache_alloc(g_kmalloc_caches[i], flag);
        }
    }
    return NULL;
}

int mm_slub_kfree(void *ptr)
{
    struct mm_slab_hdr *slab;

    if (ptr == NULL) {
        return 0;
    }
    slab = ptr_to_slab(ptr);
    if (slab->magic != MM_SLAB_MAGIC || slab->cache == NULL) {
        return -EINVAL;
    }
    return kmem_cache_free(slab->cache, ptr);
}

size_t mm_slub_obj_size(const void *ptr)
{
    const struct mm_slab_hdr *slab;

    if (ptr == NULL) {
        return 0U;
    }
    slab = ptr_to_slab(ptr);
    if (slab->magic != MM_SLAB_MAGIC || slab->cache == NULL) {
        return 0U;
    }
    return slab->cache->obj_size;
}
