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

#define SEV		__ALT_SMP_ASM(WASM(sev), WASM(nop))

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
        pr_err("next=%u, owner=%u\r\n", lockval.tickets.next, lockval.tickets.owner);
        __asm__ volatile ("wfe");
        lockval.tickets.owner = ACCESS_ONCE(lock->tickets.owner);
    }

    smp_mb();
}

static inline void arch_spin_unlock(arch_spinlock_t *lock)
{
    smp_mb();
    lock->tickets.owner++;
    dsb_sev();
}

#endif /* __ARM_ASM_SPINLOCK_H__ */
