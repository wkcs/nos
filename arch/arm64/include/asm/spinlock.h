/**
 * Copyright (C) 2023-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __ARM64_ASM_SPINLOCK_H__
#define __ARM64_ASM_SPINLOCK_H__

#include <kernel/kernel.h>
#include <asm/barrier.h>

#define TICKET_SHIFT 16
#define ARCH_HAS_SPIN_TRYLOCK 1

typedef struct {
    union {
        u32 slock;
        struct __raw_tickets {
#ifdef __AARCH64EB__
            u16 next;
            u16 owner;
#else
            u16 owner;
            u16 next;
#endif
        } tickets;
    };
} arch_spinlock_t;

static inline int arch_spin_is_locked(const arch_spinlock_t *lock)
{
    return ACCESS_ONCE(lock->tickets.next) != ACCESS_ONCE(lock->tickets.owner);
}

/* Stub ticket lock: contract-compatible with arm; refine with LSE later. */
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

    lockval.slock = old;
    while (lockval.tickets.next != lockval.tickets.owner) {
        wfe();
        lockval.tickets.owner = ACCESS_ONCE(lock->tickets.owner);
    }

    smp_mb();
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
    if (__atomic_compare_exchange_n(&lock->slock, &old, newval,
                                    0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        smp_mb();
        return 1;
    }
    return 0;
}

static inline void arch_spin_unlock(arch_spinlock_t *lock)
{
    smp_mb();
    lock->tickets.owner++;
    dsb(ishst);
    sev();
}

#endif /* __ARM64_ASM_SPINLOCK_H__ */
