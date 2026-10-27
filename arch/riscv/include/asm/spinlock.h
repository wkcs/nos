/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 */

#ifndef __RISCV_ASM_SPINLOCK_H__
#define __RISCV_ASM_SPINLOCK_H__

#include <kernel/kernel.h>
#include <asm/barrier.h>

typedef struct {
    volatile u32 slock;
} arch_spinlock_t;

static inline void arch_spin_lock(arch_spinlock_t *lock)
{
    while (__atomic_test_and_set(&lock->slock, __ATOMIC_ACQUIRE)) {
        nop();
    }
}

static inline void arch_spin_unlock(arch_spinlock_t *lock)
{
    __atomic_clear(&lock->slock, __ATOMIC_RELEASE);
}

#endif /* __RISCV_ASM_SPINLOCK_H__ */
