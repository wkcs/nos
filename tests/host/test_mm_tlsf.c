/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <kernel/mm.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THREADS 8
#define ROUNDS  2500

static struct mm_tlsf_pool g_pool;
static uint8_t g_arena[256 * 1024] __attribute__((aligned(4096)));

struct live_slot {
    char *ptr;
    size_t size;
};

static struct live_slot g_live[THREADS];
static pthread_mutex_t g_live_lock = PTHREAD_MUTEX_INITIALIZER;

static void test_unaligned_tail_utilization(void)
{
    /* Simulate a 20,123-byte unaligned MCU SRAM pool (like STM32F103 20KB minus data/bss) */
    uint8_t *raw = malloc(20123 + 16);
    struct mm_tlsf_pool pool;
    void *big;
    size_t max_blk;

    assert(raw != NULL);
    assert(mm_tlsf_init(&pool, (addr_t)(raw + 3), 20123) == 0);
    /* Must not truncate down to 4096B multiples (16384B); over 19.9KB must be usable */
    assert(mm_tlsf_total_size(&pool) >= 20000);
    max_blk = mm_tlsf_max_free_block(&pool);
    assert(max_blk >= 20000);

    big = mm_tlsf_alloc(&pool, max_blk, 8, 1);
    assert(big != NULL);
    assert(mm_tlsf_used_size(&pool) >= max_blk);
    assert(mm_tlsf_free(&pool, big) == 0);
    assert(mm_tlsf_used_size(&pool) == 0);
    assert(mm_tlsf_max_free_block(&pool) == max_blk);
    free(raw);
}

static void test_zero_fragmentation_coalescing(void)
{
    void *ptrs[64];
    size_t full_cap;
    void *whole;

    assert(mm_tlsf_init(&g_pool, (addr_t)g_arena, 65536) == 0);
    full_cap = mm_tlsf_max_free_block(&g_pool);

    for (int i = 0; i < 64; i++) {
        size_t sz = (size_t)(32 + (i % 7) * 48);
        ptrs[i] = mm_tlsf_alloc(&g_pool, sz, 8, (pid_t)(10 + (i % 4)));
        assert(ptrs[i] != NULL);
        memset(ptrs[i], (int)(0xA0 + i), sz);
    }

    /* Free even indices first (creating 32 isolated holes), then odd indices */
    for (int i = 0; i < 64; i += 2) {
        assert(mm_tlsf_free(&g_pool, ptrs[i]) == 0);
    }
    for (int i = 1; i < 64; i += 2) {
        assert(mm_tlsf_free(&g_pool, ptrs[i]) == 0);
    }

    assert(mm_tlsf_used_size(&g_pool) == 0);
    assert(mm_tlsf_max_free_block(&g_pool) == full_cap);

    /* Must be able to allocate the entire arena in one piece after coalescing */
    whole = mm_tlsf_alloc(&g_pool, full_cap, 8, 1);
    assert(whole != NULL);
    assert(mm_tlsf_free(&g_pool, whole) == 0);
}

static void test_aligned_allocation_and_gap_recycling(void)
{
    size_t full_cap;
    void *p_small, *p_mpu32, *p_page4k, *whole;

    assert(mm_tlsf_init(&g_pool, (addr_t)g_arena, 65536) == 0);
    full_cap = mm_tlsf_max_free_block(&g_pool);

    p_small = mm_tlsf_alloc(&g_pool, 24, 8, 1);
    p_mpu32 = mm_tlsf_alloc(&g_pool, 1024, 32, 1);
    p_page4k = mm_tlsf_alloc(&g_pool, 4096, 4096, 1);

    assert(p_small != NULL);
    assert(p_mpu32 != NULL && ((uintptr_t)p_mpu32 & 31U) == 0U);
    assert(p_page4k != NULL && ((uintptr_t)p_page4k & 4095U) == 0U);

    memset(p_mpu32, 0x3C, 1024);
    memset(p_page4k, 0x7E, 4096);

    assert(mm_tlsf_free(&g_pool, p_mpu32) == 0);
    assert(mm_tlsf_free(&g_pool, p_small) == 0);
    assert(mm_tlsf_free(&g_pool, p_page4k) == 0);

    /* Verify leading alignment gap was properly coalesced back into the pool */
    assert(mm_tlsf_used_size(&g_pool) == 0);
    assert(mm_tlsf_max_free_block(&g_pool) == full_cap);
    whole = mm_tlsf_alloc(&g_pool, full_cap, 8, 1);
    assert(whole != NULL);
    assert(mm_tlsf_free(&g_pool, whole) == 0);
}

static void test_inplace_realloc_and_free_by_pid(void)
{
    char *a, *b, *c;

    assert(mm_tlsf_init(&g_pool, (addr_t)g_arena, 65536) == 0);
    a = mm_tlsf_alloc(&g_pool, 64, 8, 77);
    b = mm_tlsf_alloc(&g_pool, 128, 8, 88);
    c = mm_tlsf_alloc(&g_pool, 64, 8, 77);
    assert(a && b && c);

    memset(a, 0x5A, 64);
    /* Free 'b' so 'a' has a free neighbor right after it */
    assert(mm_tlsf_free(&g_pool, b) == 0);

    /* Realloc 'a' from 64 -> 128 bytes: should expand in-place without moving 'a' */
    char *a_expanded = mm_tlsf_realloc(&g_pool, a, 128, 77);
    assert(a_expanded == a);
    for (int i = 0; i < 64; i++) {
        assert((unsigned char)a_expanded[i] == 0x5A);
    }

    /* Free all allocations belonging to pid 77 */
    assert(mm_tlsf_free_by_pid(&g_pool, 77) == 0);
    assert(mm_tlsf_used_size(&g_pool) == 0);
}

static void *concurrent_worker(void *arg)
{
    uintptr_t id = (uintptr_t)arg;
    for (int r = 0; r < ROUNDS; r++) {
        size_t sz = 1 + ((r * 53 + id * 97) % 1024);
        size_t align = (r % 16 == 0) ? 32 : 8;
        unsigned char pat = (unsigned char)(id + 1);
        char *p = mm_tlsf_alloc(&g_pool, sz, align, (pid_t)(id + 1));
        assert(p != NULL);
        assert(((uintptr_t)p & (align - 1)) == 0);

        pthread_mutex_lock(&g_live_lock);
        for (int j = 0; j < THREADS; j++) {
            if (g_live[j].ptr != NULL) {
                assert((uintptr_t)p + sz <= (uintptr_t)g_live[j].ptr ||
                       (uintptr_t)g_live[j].ptr + g_live[j].size <= (uintptr_t)p);
            }
        }
        g_live[id] = (struct live_slot){ p, sz };
        pthread_mutex_unlock(&g_live_lock);

        memset(p, pat, sz);
        sched_yield();
        for (size_t i = 0; i < sz; i++) {
            assert((unsigned char)p[i] == pat);
        }

        pthread_mutex_lock(&g_live_lock);
        g_live[id].ptr = NULL;
        assert(mm_tlsf_free(&g_pool, p) == 0);
        pthread_mutex_unlock(&g_live_lock);
    }
    return NULL;
}

int main(void)
{
    pthread_t threads[THREADS];
    size_t full_cap;

    test_unaligned_tail_utilization();
    test_zero_fragmentation_coalescing();
    test_aligned_allocation_and_gap_recycling();
    test_inplace_realloc_and_free_by_pid();

    assert(mm_tlsf_init(&g_pool, (addr_t)g_arena, sizeof(g_arena)) == 0);
    full_cap = mm_tlsf_max_free_block(&g_pool);
    for (uintptr_t i = 0; i < THREADS; i++) {
        assert(pthread_create(&threads[i], NULL, concurrent_worker, (void *)i) == 0);
    }
    for (int i = 0; i < THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    assert(mm_tlsf_used_size(&g_pool) == 0);
    assert(mm_tlsf_max_free_block(&g_pool) == full_cap);
    puts("TLSF allocator checks passed");
    return 0;
}
