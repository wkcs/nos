/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 原子操作实现（基于内建原子语义，支持 LSE/LL-SC）
 */

#include <asm/atomic.h>

void atomic_add(int i, atomic_t *v)
{
    if (v != NULL) {
        __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST);
    }
}

void atomic_sub(int i, atomic_t *v)
{
    if (v != NULL) {
        __atomic_sub_fetch(&v->counter, i, __ATOMIC_SEQ_CST);
    }
}

void atomic_inc(atomic_t *v)
{
    atomic_add(1, v);
}

void atomic_dec(atomic_t *v)
{
    atomic_sub(1, v);
}

int atomic_cmpxchg(atomic_t *v, int old, int new_val)
{
    int expected = old;
    if (v == NULL) {
        return 0;
    }
    __atomic_compare_exchange_n(&v->counter, &expected, new_val,
                                false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}

int atomic_test_and_set(atomic_t *v)
{
    if (v == NULL) {
        return 0;
    }
    return __atomic_exchange_n(&v->counter, 1, __ATOMIC_ACQ_REL);
}
