/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __ARM_ASM_SPINLOCK_H__
#define __ARM_ASM_SPINLOCK_H__

#include <kernel/kernel.h>
#include <asm/barrier.h>

#define TICKET_SHIFT 16
#define ARCH_HAS_SPIN_TRYLOCK 1

typedef struct {
    union {
        u32 slock;
        struct __raw_tickets {
#ifdef __ARMEB__
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

#if !defined(CONFIG_SMP) && (!defined(CONFIG_NR_CPUS) || (CONFIG_NR_CPUS <= 1))

static inline void arch_spin_lock(arch_spinlock_t *lock)
{
    lock->tickets.next = (u16)(lock->tickets.owner + 1U);
    barrier();
}

static inline int arch_spin_trylock(arch_spinlock_t *lock)
{
    if (arch_spin_is_locked(lock)) {
        return 0;
    }
    lock->tickets.next = (u16)(lock->tickets.owner + 1U);
    barrier();
    return 1;
}

static inline void arch_spin_unlock(arch_spinlock_t *lock)
{
    barrier();
    lock->tickets.owner = lock->tickets.next;
}

#else /* SMP */

#define SEV __ALT_SMP_ASM(WASM(sev), WASM(nop))

static inline void dsb_sev(void)
{

    // dsb(ishst);
    __asm__ volatile ("dsb 0xF":::"memory");
    __asm__(SEV);
}

static inline void arch_spin_lock(arch_spinlock_t *lock)
{
    unsigned long tmp;
    u32 newval;
    arch_spinlock_t lockval;

    // prefetchw(&lock->slock);
    __asm__ __volatile__(
"1:	ldrex	%0, [%3]\n"
"	add	%1, %0, %4\n"
"	strex	%2, %1, [%3]\n"
"	teq	%2, #0\n"
"	bne	1b"
    : "=&r" (lockval), "=&r" (newval), "=&r" (tmp)
    : "r" (&lock->slock), "I" (1 << TICKET_SHIFT)
    : "cc");

    while (lockval.tickets.next != lockval.tickets.owner) {
        __asm__ volatile ("wfe");
        lockval.tickets.owner = ACCESS_ONCE(lock->tickets.owner);
    }

    smp_mb();
}

static inline int arch_spin_trylock(arch_spinlock_t *lock)
{
    unsigned long contended, res;
    u32 slock;

    do {
        __asm__ __volatile__(
        "   ldrex   %0, [%3]\n"
        "   mov     %2, #0\n"
        "   subs    %1, %0, %0, ror #16\n"
        "   addeq   %0, %0, %4\n"
        "   strexeq %2, %0, [%3]"
        : "=&r" (slock), "=&r" (contended), "=&r" (res)
        : "r" (&lock->slock), "I" (1 << TICKET_SHIFT)
        : "cc");
    } while (res);

    if (!contended) {
        smp_mb();
        return 1;
    }
    return 0;
}

static inline void arch_spin_unlock(arch_spinlock_t *lock)
{
    smp_mb();
    lock->tickets.owner++;
    dsb_sev();
}

#endif /* CONFIG_SMP */

#endif /* __ARM_ASM_SPINLOCK_H__ */
