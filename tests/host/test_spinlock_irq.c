/**
 * Copyright (C) 2026 Nick Hu
 *
 * A waiting CPU must not overwrite the lock owner's saved interrupt state.
 */

#include <stdbool.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include "../../include/kernel/spinlock.h"

_Thread_local int test_lock_role;
static _Thread_local addr_t test_irq_state;
int test_waiter_entered;
static int owner_entered;
static int owner_irq_state = -1;
static SPINLOCK(test_lock);
static SPINLOCK(lock_a);
static SPINLOCK(lock_b);

addr_t disable_irq_save(void)
{
    addr_t old = test_irq_state;
    test_irq_state = 1;
    return old;
}

void enable_irq_save(addr_t flags)
{
    test_irq_state = flags;
}

static void *owner_thread(void *unused)
{
    (void)unused;
    test_lock_role = 1;
    test_irq_state = 0;
    spin_lock_irq(&test_lock);
    __atomic_store_n(&owner_entered, 1, __ATOMIC_SEQ_CST);
    while (!__atomic_load_n(&test_waiter_entered, __ATOMIC_SEQ_CST)) sched_yield();
    spin_unlock_irq(&test_lock);
    owner_irq_state = (int)test_irq_state;
    return NULL;
}

static void *waiter_thread(void *unused)
{
    (void)unused;
    test_lock_role = 2;
    test_irq_state = 1;
    while (!__atomic_load_n(&owner_entered, __ATOMIC_SEQ_CST)) sched_yield();
    spin_lock_irq(&test_lock);
    spin_unlock_irq(&test_lock);
    return NULL;
}

int main(void)
{
    pthread_t owner;
    pthread_t waiter;
    if (pthread_create(&owner, NULL, owner_thread, NULL) != 0 ||
        pthread_create(&waiter, NULL, waiter_thread, NULL) != 0) return 2;
    pthread_join(owner, NULL);
    pthread_join(waiter, NULL);
    if (owner_irq_state != 0) {
        fputs("FAIL: waiter overwrote owner's saved interrupt state\n", stderr);
        return 1;
    }

    /* Verify LIFO nested spin_lock_irq / spin_unlock_irq */
    test_irq_state = 0;
    spin_lock_irq(&lock_a);
    if (test_irq_state != 1 || !spin_is_locked(&lock_a)) {
        fputs("FAIL: lock_a did not disable IRQ or mark locked\n", stderr);
        return 1;
    }
    spin_lock_irq(&lock_b);
    if (test_irq_state != 1 || !spin_is_locked(&lock_b)) {
        fputs("FAIL: lock_b did not keep IRQ disabled\n", stderr);
        return 1;
    }
    spin_unlock_irq(&lock_b);
    if (test_irq_state != 1 || spin_is_locked(&lock_b)) {
        fputs("FAIL: LIFO inner unlock prematurely re-enabled IRQ\n", stderr);
        return 1;
    }
    spin_unlock_irq(&lock_a);
    if (test_irq_state != 0 || spin_is_locked(&lock_a)) {
        fputs("FAIL: LIFO outer unlock did not restore IRQ state\n", stderr);
        return 1;
    }

    /* Verify non-LIFO nested spin_lock_irq(A); spin_lock_irq(B); spin_unlock_irq(A); spin_unlock_irq(B); */
    test_irq_state = 0;
    spin_lock_irq(&lock_a);
    spin_lock_irq(&lock_b);
    spin_unlock_irq(&lock_a);
    if (test_irq_state != 1 || !spin_is_locked(&lock_b)) {
        fputs("FAIL: non-LIFO first unlock prematurely re-enabled IRQ while lock_b held\n", stderr);
        return 1;
    }
    spin_unlock_irq(&lock_b);
    if (test_irq_state != 0 || spin_is_locked(&lock_b)) {
        fputs("FAIL: non-LIFO last unlock failed to restore outermost IRQ state\n", stderr);
        return 1;
    }

    /* Verify spin_trylock_irq and spin_lock_irqsave / spin_unlock_irqrestore */
    {
        addr_t flags = 0;
        test_irq_state = 0;
        if (!spin_trylock_irq(&lock_a)) {
            fputs("FAIL: spin_trylock_irq failed on unlocked lock_a\n", stderr);
            return 1;
        }
        if (spin_trylock_irq(&lock_a)) {
            fputs("FAIL: spin_trylock_irq succeeded on already-locked lock_a\n", stderr);
            return 1;
        }
        spin_lock_irqsave(&lock_b, flags);
        spin_unlock_irq(&lock_a);
        if (test_irq_state != 1) {
            fputs("FAIL: IRQ re-enabled while irqsave lock_b still held\n", stderr);
            return 1;
        }
        spin_unlock_irqrestore(&lock_b, flags);
        if (test_irq_state != 0) {
            fputs("FAIL: spin_unlock_irqrestore did not restore IRQ state\n", stderr);
            return 1;
        }
    }

    puts("spinlock IRQ state checks passed");
    return 0;
}
