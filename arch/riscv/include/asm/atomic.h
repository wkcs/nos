/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 */

#ifndef __ASM_RISCV_ATOMIC_H__
#define __ASM_RISCV_ATOMIC_H__

#include <kernel/types.h>

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

#endif /* __ASM_RISCV_ATOMIC_H__ */
