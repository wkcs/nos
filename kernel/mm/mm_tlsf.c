/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * Two-Level Segregated Fit (TLSF) O(1) Low-Fragmentation Memory Allocator
 */

#define pr_fmt(fmt) "[MM_TLSF]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/mm.h>
#include <kernel/kernel.h>
#include <kernel/printk.h>
#include <string.h>

#define TLSF_FLAG_FREE      ((size_t)1U << 0)
#define TLSF_FLAG_PREV_FREE ((size_t)1U << 1)
#define TLSF_SIZE_MASK      (~((size_t)MM_TLSF_ALIGN - 1U))

struct tlsf_free_links {
    struct mm_tlsf_block *next_free;
    struct mm_tlsf_block *prev_free;
};

static inline size_t tlsf_align_up(size_t x, size_t align)
{
    return (x + align - 1U) & ~(align - 1U);
}

static inline size_t tlsf_align_down(size_t x, size_t align)
{
    return x & ~(align - 1U);
}

static inline size_t block_size(const struct mm_tlsf_block *blk)
{
    return blk->size_flags & TLSF_SIZE_MASK;
}

static inline void block_set_size(struct mm_tlsf_block *blk, size_t size)
{
    blk->size_flags = (size & TLSF_SIZE_MASK) |
                      (blk->size_flags & (TLSF_FLAG_FREE | TLSF_FLAG_PREV_FREE));
}

static inline bool block_is_free(const struct mm_tlsf_block *blk)
{
    return (blk->size_flags & TLSF_FLAG_FREE) != 0U;
}

static inline bool block_is_prev_free(const struct mm_tlsf_block *blk)
{
    return (blk->size_flags & TLSF_FLAG_PREV_FREE) != 0U;
}

static inline bool block_is_last(const struct mm_tlsf_block *blk)
{
    return block_size(blk) == 0U;
}

static inline void *block_to_ptr(const struct mm_tlsf_block *blk)
{
    return (void *)((uintptr_t)blk + sizeof(struct mm_tlsf_block));
}

static inline struct mm_tlsf_block *ptr_to_block(const void *ptr)
{
    return (struct mm_tlsf_block *)((uintptr_t)ptr - sizeof(struct mm_tlsf_block));
}

static inline struct tlsf_free_links *block_links(struct mm_tlsf_block *blk)
{
    return (struct tlsf_free_links *)block_to_ptr(blk);
}

static inline struct mm_tlsf_block *block_next_phys(const struct mm_tlsf_block *blk)
{
    return (struct mm_tlsf_block *)((uintptr_t)block_to_ptr(blk) + block_size(blk));
}

static inline u32 tlsf_fls(size_t size)
{
    u32 val = (u32)(size > 0xFFFFFFFFUL ? 0xFFFFFFFFUL : size);
    if (val == 0U) {
        return 0U;
    }
    return 31U - (u32)__builtin_clz(val);
}

static inline u32 tlsf_ffs(u32 mask)
{
    return (u32)__builtin_ctz(mask);
}

static void mapping_insert(size_t size, u32 *out_fl, u32 *out_sl)
{
    u32 fl, sl;

    if (size < MM_TLSF_MIN_BLOCK_SIZE) {
        *out_fl = 0U;
        *out_sl = 0U;
        return;
    }
    fl = tlsf_fls(size);
    if (fl > MM_TLSF_FL_MAX) {
        fl = MM_TLSF_FL_MAX;
        sl = MM_TLSF_SL_COUNT - 1U;
    } else {
        sl = (u32)((size >> (fl - MM_TLSF_SL_LOG2)) ^ (1UL << MM_TLSF_SL_LOG2));
    }
    *out_fl = fl - MM_TLSF_FL_SHIFT;
    *out_sl = sl;
}

static void remove_free_block(struct mm_tlsf_pool *pool, struct mm_tlsf_block *blk)
{
    u32 fl, sl;
    struct tlsf_free_links *links = block_links(blk);
    struct mm_tlsf_block *prev = links->prev_free;
    struct mm_tlsf_block *next = links->next_free;

    mapping_insert(block_size(blk), &fl, &sl);
    if (next != NULL) {
        block_links(next)->prev_free = prev;
    }
    if (prev != NULL) {
        block_links(prev)->next_free = next;
    }
    if (pool->blocks[fl][sl] == blk) {
        pool->blocks[fl][sl] = next;
        if (next == NULL) {
            pool->sl_bitmap[fl] &= ~(1U << sl);
            if (pool->sl_bitmap[fl] == 0U) {
                pool->fl_bitmap &= ~(1U << fl);
            }
        }
    }
    links->next_free = NULL;
    links->prev_free = NULL;
}

static void insert_free_block(struct mm_tlsf_pool *pool, struct mm_tlsf_block *blk)
{
    u32 fl, sl;
    struct mm_tlsf_block *head;
    struct tlsf_free_links *links = block_links(blk);

    mapping_insert(block_size(blk), &fl, &sl);
    head = pool->blocks[fl][sl];
    links->next_free = head;
    links->prev_free = NULL;
    if (head != NULL) {
        block_links(head)->prev_free = blk;
    }
    pool->blocks[fl][sl] = blk;
    pool->fl_bitmap |= (1U << fl);
    pool->sl_bitmap[fl] |= (1U << sl);
}

static void mark_block_free(struct mm_tlsf_block *blk)
{
    struct mm_tlsf_block *next = block_next_phys(blk);
    blk->size_flags |= TLSF_FLAG_FREE;
    blk->pid = 0;
    next->prev_phys = blk;
    next->size_flags |= TLSF_FLAG_PREV_FREE;
}

static void mark_block_used(struct mm_tlsf_block *blk, pid_t pid)
{
    struct mm_tlsf_block *next = block_next_phys(blk);
    blk->size_flags &= ~TLSF_FLAG_FREE;
    blk->pid = pid;
    next->size_flags &= ~TLSF_FLAG_PREV_FREE;
}

static struct mm_tlsf_block *find_suitable_block(struct mm_tlsf_pool *pool, size_t req_size)
{
    u32 fl, sl;
    u32 sl_map, fl_map;
    struct mm_tlsf_block *cur;
    int scan = 0;

    mapping_insert(req_size, &fl, &sl);

    /* Check exact bucket first (bounded O(1) scan of up to 8 blocks) */
    cur = pool->blocks[fl][sl];
    while (cur != NULL && scan < 8) {
        if (block_size(cur) >= req_size) {
            return cur;
        }
        cur = block_links(cur)->next_free;
        scan++;
    }

    /* Search strictly larger buckets via SL/FL bitmaps in O(1) */
    sl_map = pool->sl_bitmap[fl] & (~0U << (sl + 1U));
    if (sl_map == 0U) {
        fl_map = pool->fl_bitmap & (~0U << (fl + 1U));
        if (fl_map == 0U) {
            /* Final fallback: check remaining items in exact bucket if any */
            while (cur != NULL) {
                if (block_size(cur) >= req_size) {
                    return cur;
                }
                cur = block_links(cur)->next_free;
            }
            return NULL;
        }
        fl = tlsf_ffs(fl_map);
        sl_map = pool->sl_bitmap[fl];
    }
    sl = tlsf_ffs(sl_map);
    return pool->blocks[fl][sl];
}

static struct mm_tlsf_block *coalesce_prev(struct mm_tlsf_pool *pool, struct mm_tlsf_block *blk)
{
    if (block_is_prev_free(blk)) {
        struct mm_tlsf_block *prev = blk->prev_phys;
#ifdef CONFIG_MM_DEBUG
        BUG_ON(prev == NULL || prev->magic != MM_TLSF_MAGIC || !block_is_free(prev));
#endif
        remove_free_block(pool, prev);
        block_set_size(prev, block_size(prev) + sizeof(struct mm_tlsf_block) + block_size(blk));
#ifdef CONFIG_MM_DEBUG
        blk->magic = 0U;
#endif
        blk = prev;
    }
    return blk;
}

static struct mm_tlsf_block *coalesce_next(struct mm_tlsf_pool *pool, struct mm_tlsf_block *blk)
{
    struct mm_tlsf_block *next = block_next_phys(blk);
    if (!block_is_last(next) && block_is_free(next)) {
#ifdef CONFIG_MM_DEBUG
        BUG_ON(next->magic != MM_TLSF_MAGIC);
#endif
        remove_free_block(pool, next);
        block_set_size(blk, block_size(blk) + sizeof(struct mm_tlsf_block) + block_size(next));
#ifdef CONFIG_MM_DEBUG
        next->magic = 0U;
#endif
    }
    return blk;
}

static void trim_trailing_space(struct mm_tlsf_pool *pool,
                                struct mm_tlsf_block *blk,
                                size_t req_size)
{
    size_t cur_size = block_size(blk);
    size_t min_split = sizeof(struct mm_tlsf_block) + MM_TLSF_MIN_BLOCK_SIZE;

    if (cur_size >= req_size + min_split) {
        struct mm_tlsf_block *rem;
        size_t rem_size = cur_size - req_size - sizeof(struct mm_tlsf_block);

        block_set_size(blk, req_size);
        rem = block_next_phys(blk);
        rem->prev_phys = blk;
        rem->size_flags = rem_size | TLSF_FLAG_FREE;
        rem->pid = 0;
        rem->magic = MM_TLSF_MAGIC;
        mark_block_free(rem);
        rem = coalesce_next(pool, rem);
        mark_block_free(rem);
        insert_free_block(pool, rem);
    }
}

int mm_tlsf_init(struct mm_tlsf_pool *pool, addr_t start, size_t size)
{
    addr_t aligned_start;
    addr_t aligned_end;
    size_t usable_bytes;
    size_t body_size;
    struct mm_tlsf_block *first;
    struct mm_tlsf_block *epilogue;

    if (pool == NULL || start == 0U) {
        return -EINVAL;
    }

    aligned_start = (addr_t)tlsf_align_up(start, MM_TLSF_ALIGN);
    aligned_end = (addr_t)tlsf_align_down(start + size, MM_TLSF_ALIGN);
    if (aligned_end <= aligned_start) {
        return -ENOMEM;
    }

    usable_bytes = (size_t)(aligned_end - aligned_start);
    if (usable_bytes < 2U * sizeof(struct mm_tlsf_block) + MM_TLSF_MIN_BLOCK_SIZE) {
        return -ENOMEM;
    }

    memset(pool, 0, sizeof(*pool));
    spin_lock_init(&pool->lock);
    pool->start = aligned_start;
    pool->end = aligned_end;

    body_size = usable_bytes - 2U * sizeof(struct mm_tlsf_block);
    pool->total_size = body_size;
    pool->used_size = 0U;

    first = (struct mm_tlsf_block *)aligned_start;
    first->prev_phys = NULL;
    first->size_flags = body_size | TLSF_FLAG_FREE;
    first->pid = 0;
    first->magic = MM_TLSF_MAGIC;

    epilogue = block_next_phys(first);
    epilogue->prev_phys = first;
    epilogue->size_flags = TLSF_FLAG_PREV_FREE; /* size == 0 sentinel */
    epilogue->pid = 0;
    epilogue->magic = MM_TLSF_MAGIC;

    pool->first_block = first;
    pool->initialized = true;
    insert_free_block(pool, first);
    return 0;
}

void *mm_tlsf_alloc(struct mm_tlsf_pool *pool, size_t size, size_t align, pid_t pid)
{
    size_t req_size;
    size_t search_size;
    struct mm_tlsf_block *blk;
    uintptr_t ptr;

    if (pool == NULL || !pool->initialized || size == 0U) {
        return NULL;
    }
    if (align < MM_TLSF_ALIGN) {
        align = MM_TLSF_ALIGN;
    }
    if ((align & (align - 1U)) != 0U) {
        return NULL;
    }
    if (size > SIZE_MAX - (align + 2U * sizeof(struct mm_tlsf_block) + MM_TLSF_MIN_BLOCK_SIZE)) {
        return NULL;
    }

    req_size = tlsf_align_up(size < MM_TLSF_MIN_BLOCK_SIZE ? MM_TLSF_MIN_BLOCK_SIZE : size,
                             MM_TLSF_ALIGN);
    search_size = req_size;
    if (align > MM_TLSF_ALIGN) {
        search_size += align + sizeof(struct mm_tlsf_block) + MM_TLSF_MIN_BLOCK_SIZE;
    }

    spin_lock_irq(&pool->lock);
    blk = find_suitable_block(pool, req_size);
    if (blk != NULL && align > MM_TLSF_ALIGN) {
        uintptr_t raw_ptr = (uintptr_t)block_to_ptr(blk);
        if ((raw_ptr & (align - 1U)) != 0U) {
            uintptr_t min_aligned = tlsf_align_up(
                raw_ptr + sizeof(struct mm_tlsf_block) + MM_TLSF_MIN_BLOCK_SIZE, align);
            size_t gap = (size_t)(min_aligned - raw_ptr);
            if (block_size(blk) < req_size + gap) {
                blk = find_suitable_block(pool, search_size);
            }
        }
    }
    if (blk == NULL) {
        spin_unlock_irq(&pool->lock);
        return NULL;
    }

#ifdef CONFIG_MM_DEBUG
    BUG_ON(blk->magic != MM_TLSF_MAGIC || !block_is_free(blk));
#endif
    remove_free_block(pool, blk);

    ptr = (uintptr_t)block_to_ptr(blk);
    if (align > MM_TLSF_ALIGN && (ptr & (align - 1U)) != 0U) {
        uintptr_t aligned_ptr = tlsf_align_up(
            ptr + sizeof(struct mm_tlsf_block) + MM_TLSF_MIN_BLOCK_SIZE, align);
        size_t gap = (size_t)(aligned_ptr - ptr);
        size_t orig_size = block_size(blk);
        struct mm_tlsf_block *aligned_blk =
            (struct mm_tlsf_block *)(aligned_ptr - sizeof(struct mm_tlsf_block));

        block_set_size(blk, gap - sizeof(struct mm_tlsf_block));
        aligned_blk->prev_phys = blk;
        aligned_blk->size_flags = (orig_size - gap) | TLSF_FLAG_FREE | TLSF_FLAG_PREV_FREE;
        aligned_blk->pid = 0;
        aligned_blk->magic = MM_TLSF_MAGIC;

        mark_block_free(blk);
        insert_free_block(pool, blk);
        blk = aligned_blk;
    }

    trim_trailing_space(pool, blk, req_size);
    mark_block_used(blk, pid);
    pool->used_size += block_size(blk);
    spin_unlock_irq(&pool->lock);

    return block_to_ptr(blk);
}

int mm_tlsf_free(struct mm_tlsf_pool *pool, void *ptr)
{
    struct mm_tlsf_block *blk;

    if (ptr == NULL) {
        return 0;
    }
    if (pool == NULL || !pool->initialized || !mm_tlsf_owns(pool, ptr)) {
        return -EINVAL;
    }

    blk = ptr_to_block(ptr);
    spin_lock_irq(&pool->lock);
    if (blk->magic != MM_TLSF_MAGIC || block_is_free(blk)) {
        spin_unlock_irq(&pool->lock);
        return -EINVAL;
    }

    pool->used_size -= block_size(blk);
    mark_block_free(blk);
    blk = coalesce_prev(pool, blk);
    blk = coalesce_next(pool, blk);
    mark_block_free(blk);
    insert_free_block(pool, blk);
    spin_unlock_irq(&pool->lock);
    return 0;
}

void *mm_tlsf_realloc(struct mm_tlsf_pool *pool, void *ptr, size_t new_size, pid_t pid)
{
    struct mm_tlsf_block *blk;
    struct mm_tlsf_block *next;
    size_t cur_size;
    size_t req_size;
    void *new_ptr;

    if (pool == NULL || !pool->initialized) {
        return NULL;
    }
    if (new_size == 0U) {
        if (ptr != NULL) {
            (void)mm_tlsf_free(pool, ptr);
        }
        return NULL;
    }
    if (ptr == NULL) {
        return mm_tlsf_alloc(pool, new_size, MM_TLSF_ALIGN, pid);
    }
    if (!mm_tlsf_owns(pool, ptr)) {
        return NULL;
    }
    if (new_size > SIZE_MAX - (2U * sizeof(struct mm_tlsf_block) + MM_TLSF_MIN_BLOCK_SIZE)) {
        return NULL;
    }

    req_size = tlsf_align_up(
        new_size < MM_TLSF_MIN_BLOCK_SIZE ? MM_TLSF_MIN_BLOCK_SIZE : new_size,
        MM_TLSF_ALIGN);

    spin_lock_irq(&pool->lock);
    blk = ptr_to_block(ptr);
    if (blk->magic != MM_TLSF_MAGIC || block_is_free(blk)) {
        spin_unlock_irq(&pool->lock);
        return NULL;
    }
    cur_size = block_size(blk);
    if (req_size <= cur_size) {
        pool->used_size -= cur_size;
        trim_trailing_space(pool, blk, req_size);
        mark_block_used(blk, blk->pid);
        pool->used_size += block_size(blk);
        spin_unlock_irq(&pool->lock);
        return ptr;
    }

    /* Attempt in-place expansion with physical next neighbor if free */
    next = block_next_phys(blk);
    if (!block_is_last(next) && block_is_free(next) &&
        cur_size + sizeof(struct mm_tlsf_block) + block_size(next) >= req_size) {
        remove_free_block(pool, next);
        pool->used_size -= cur_size;
        block_set_size(blk, cur_size + sizeof(struct mm_tlsf_block) + block_size(next));
#ifdef CONFIG_MM_DEBUG
        next->magic = 0U;
#endif
        trim_trailing_space(pool, blk, req_size);
        mark_block_used(blk, blk->pid);
        pool->used_size += block_size(blk);
        spin_unlock_irq(&pool->lock);
        return ptr;
    }
    spin_unlock_irq(&pool->lock);

    new_ptr = mm_tlsf_alloc(pool, new_size, MM_TLSF_ALIGN, pid);
    if (new_ptr == NULL) {
        return NULL;
    }
    memcpy(new_ptr, ptr, cur_size < new_size ? cur_size : new_size);
    (void)mm_tlsf_free(pool, ptr);
    return new_ptr;
}

int mm_tlsf_free_by_pid(struct mm_tlsf_pool *pool, pid_t pid)
{
    struct mm_tlsf_block *blk;
    bool any_found = false;

    if (pool == NULL || !pool->initialized) {
        return -ENODEV;
    }

    spin_lock_irq(&pool->lock);
    blk = pool->first_block;
    while (blk != NULL && !block_is_last(blk)) {
        if (!block_is_free(blk) && blk->pid == pid) {
            any_found = true;
            pool->used_size -= block_size(blk);
            mark_block_free(blk);
            blk = coalesce_prev(pool, blk);
            blk = coalesce_next(pool, blk);
            mark_block_free(blk);
            insert_free_block(pool, blk);
        }
        blk = block_next_phys(blk);
    }
    spin_unlock_irq(&pool->lock);
    return any_found ? 0 : 0;
}

size_t mm_tlsf_block_size(const void *ptr)
{
    const struct mm_tlsf_block *blk;
    if (ptr == NULL) {
        return 0U;
    }
    blk = ptr_to_block(ptr);
    if (blk->magic != MM_TLSF_MAGIC || block_is_free(blk)) {
        return 0U;
    }
    return block_size(blk);
}

bool mm_tlsf_owns(const struct mm_tlsf_pool *pool, const void *ptr)
{
    uintptr_t addr = (uintptr_t)ptr;
    if (pool == NULL || !pool->initialized || ptr == NULL) {
        return false;
    }
    return addr >= pool->start + sizeof(struct mm_tlsf_block) &&
           addr < pool->end - sizeof(struct mm_tlsf_block);
}

size_t mm_tlsf_total_size(struct mm_tlsf_pool *pool)
{
    if (pool == NULL || !pool->initialized) {
        return 0U;
    }
    return pool->total_size;
}

size_t mm_tlsf_used_size(struct mm_tlsf_pool *pool)
{
    size_t used;
    if (pool == NULL || !pool->initialized) {
        return 0U;
    }
    spin_lock_irq(&pool->lock);
    used = pool->used_size;
    spin_unlock_irq(&pool->lock);
    return used;
}

size_t mm_tlsf_free_size(struct mm_tlsf_pool *pool)
{
    size_t free_bytes = 0U;
    struct mm_tlsf_block *blk;

    if (pool == NULL || !pool->initialized) {
        return 0U;
    }
    spin_lock_irq(&pool->lock);
    blk = pool->first_block;
    while (blk != NULL && !block_is_last(blk)) {
        if (block_is_free(blk)) {
            free_bytes += block_size(blk);
        }
        blk = block_next_phys(blk);
    }
    spin_unlock_irq(&pool->lock);
    return free_bytes;
}

size_t mm_tlsf_max_free_block(struct mm_tlsf_pool *pool)
{
    size_t max_blk = 0U;
    struct mm_tlsf_block *blk;

    if (pool == NULL || !pool->initialized) {
        return 0U;
    }
    spin_lock_irq(&pool->lock);
    blk = pool->first_block;
    while (blk != NULL && !block_is_last(blk)) {
        if (block_is_free(blk) && block_size(blk) > max_blk) {
            max_blk = block_size(blk);
        }
        blk = block_next_phys(blk);
    }
    spin_unlock_irq(&pool->lock);
    return max_blk;
}
