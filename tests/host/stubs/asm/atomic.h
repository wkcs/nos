/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * 宿主端测试桩：asm/atomic.h
 */

#ifndef NOS_TEST_ARCH_ATOMIC_H
#define NOS_TEST_ARCH_ATOMIC_H

#include <kernel/types.h>

#ifndef __ASM_GENERIC_ATOMIC_H__
#define __ASM_GENERIC_ATOMIC_H__
#endif

typedef struct {
    volatile int counter;
} atomic_t;

static inline int atomic_read(const atomic_t *v)
{
    return __atomic_load_n(&v->counter, __ATOMIC_SEQ_CST);
}

static inline void atomic_set(atomic_t *v, int i)
{
    __atomic_store_n(&v->counter, i, __ATOMIC_SEQ_CST);
}

static inline void atomic_add(int i, atomic_t *v)
{
    (void)__atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST);
}

static inline void atomic_sub(int i, atomic_t *v)
{
    (void)__atomic_sub_fetch(&v->counter, i, __ATOMIC_SEQ_CST);
}

static inline void atomic_inc(atomic_t *v)
{
    atomic_add(1, v);
}

static inline void atomic_dec(atomic_t *v)
{
    atomic_sub(1, v);
}

static inline int atomic_cmpxchg(atomic_t *v, int old, int new)
{
    int expected = old;
    __atomic_compare_exchange_n(&v->counter, &expected, new, 0,
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}

static inline int atomic_test_and_set(atomic_t *v)
{
    return __atomic_exchange_n(&v->counter, 1, __ATOMIC_SEQ_CST);
}

#endif /* NOS_TEST_ARCH_ATOMIC_H */
