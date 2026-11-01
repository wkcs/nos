/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <kernel/mm.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned g_pages_live;

addr_t alloc_pages(gfp_t flag, u32 order)
{
    void *ptr = NULL;
    size_t bytes = (size_t)CONFIG_PAGE_SIZE << order;
    if (posix_memalign(&ptr, CONFIG_PAGE_SIZE, bytes) != 0) {
        return 0;
    }
    if (flag & 2) { /* GFP_ZERO */
        memset(ptr, 0, bytes);
    }
    __atomic_add_fetch(&g_pages_live, 1U << order, __ATOMIC_RELAXED);
    return (addr_t)ptr;
}

addr_t alloc_page(gfp_t flag)
{
    return alloc_pages(flag, 0);
}

int free_pages(addr_t addr, u32 order)
{
    if (addr == 0) {
        return -EINVAL;
    }
    __atomic_sub_fetch(&g_pages_live, 1U << order, __ATOMIC_RELAXED);
    free((void *)addr);
    return 0;
}

int free_page(addr_t addr)
{
    return free_pages(addr, 0);
}

static void test_dedicated_kmem_cache(void)
{
    struct kmem_cache *cache;
    void *objs[64];

    cache = kmem_cache_create("test_obj_64", 64, 16, 0);
    assert(cache != NULL);
    assert(cache->objs_per_slab >= 50); /* 4096B slab holds >50 64B objects without per-object headers */

    /* Allocate 50 objects: must fit inside a single 4096B slab page! */
    unsigned base_pages = __atomic_load_n(&g_pages_live, __ATOMIC_RELAXED);
    for (int i = 0; i < 50; i++) {
        objs[i] = kmem_cache_alloc(cache, 2 /* GFP_ZERO */);
        assert(objs[i] != NULL);
        assert(((uintptr_t)objs[i] & 15U) == 0U);
        for (int b = 0; b < 64; b++) {
            assert(((unsigned char *)objs[i])[b] == 0);
        }
        memset(objs[i], 0x33 + i, 64);
    }
    assert(__atomic_load_n(&g_pages_live, __ATOMIC_RELAXED) == base_pages + 1);

    /* Allocate 14 more to spill into a second slab page */
    for (int i = 50; i < 64; i++) {
        objs[i] = kmem_cache_alloc(cache, 0);
        assert(objs[i] != NULL);
        memset(objs[i], 0x33 + i, 64);
    }
    assert(__atomic_load_n(&g_pages_live, __ATOMIC_RELAXED) == base_pages + 2);

    /* Free all 64 objects and destroy cache: all pages must be reclaimed */
    for (int i = 0; i < 64; i++) {
        assert(kmem_cache_free(cache, objs[i]) == 0);
    }
    assert(cache->active_objs == 0);
    kmem_cache_destroy(cache);
    assert(__atomic_load_n(&g_pages_live, __ATOMIC_RELAXED) == 0);
}

static void test_general_slub_kmalloc(void)
{
    static const size_t sizes[] = { 8, 16, 24, 32, 48, 64, 128, 256, 512, 1024, 2048 };
    void *ptrs[11];

    assert(mm_slub_init() == 0);
    for (size_t i = 0; i < 11; i++) {
        ptrs[i] = mm_slub_kmalloc(sizes[i], 2 /* GFP_ZERO */);
        assert(ptrs[i] != NULL);
        assert(mm_slub_obj_size(ptrs[i]) >= sizes[i]);
        memset(ptrs[i], (int)(0x11 + i), sizes[i]);
    }
    for (size_t i = 0; i < 11; i++) {
        assert(mm_slub_kfree(ptrs[i]) == 0);
    }
}

int main(void)
{
    test_dedicated_kmem_cache();
    test_general_slub_kmalloc();
    puts("SLUB allocator checks passed");
    return 0;
}
