/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 */

#ifndef __X86_ASM_SPINLOCK_H__
#define __X86_ASM_SPINLOCK_H__

#include <kernel/kernel.h>
#include <asm/barrier.h>

#define TICKET_SHIFT 16
#define ARCH_HAS_SPIN_TRYLOCK 1

typedef struct {
    union {
        u32 slock;
        struct __raw_tickets {
            u16 owner;
            u16 next;
        } tickets;
    };
} arch_spinlock_t;

static inline int arch_spin_is_locked(const arch_spinlock_t *lock)
{
    return ACCESS_ONCE(lock->tickets.next) != ACCESS_ONCE(lock->tickets.owner);
}

static inline void arch_spin_lock(arch_spinlock_t *lock)
{
    u32 old, newval;
    arch_spinlock_t lockval;

    do {
        old = __atomic_load_n(&lock->slock, __ATOMIC_RELAXED);
        lockval.slock = old;
        newval = old + (1U << TICKET_SHIFT);
    } while (!__atomic_compare_exchange_n(&lock->slock, &old, newval,
                                          0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));

    while (lockval.tickets.next != lockval.tickets.owner) {
        nop();
        lockval.tickets.owner = ACCESS_ONCE(lock->tickets.owner);
    }
}

static inline int arch_spin_trylock(arch_spinlock_t *lock)
{
    u32 old, newval;
    arch_spinlock_t lockval;

    old = __atomic_load_n(&lock->slock, __ATOMIC_RELAXED);
    lockval.slock = old;
    if (lockval.tickets.next != lockval.tickets.owner) {
        return 0;
    }
    newval = old + (1U << TICKET_SHIFT);
    return __atomic_compare_exchange_n(&lock->slock, &old, newval,
                                       0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ? 1 : 0;
}

static inline void arch_spin_unlock(arch_spinlock_t *lock)
{
    __atomic_fetch_add(&lock->tickets.owner, 1U, __ATOMIC_RELEASE);
}

#endif /* __X86_ASM_SPINLOCK_H__ */
