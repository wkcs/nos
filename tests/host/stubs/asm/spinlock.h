/** Copyright (C) 2026 Nick Hu */
#ifndef NOS_TEST_ARCH_SPINLOCK_H
#define NOS_TEST_ARCH_SPINLOCK_H
#include <stdint.h>
#include <sched.h>
#define ARCH_HAS_SPIN_TRYLOCK 1
extern _Thread_local int test_lock_role;
extern int test_waiter_entered;
typedef union {
    uint32_t value;
    struct { uint16_t owner, next; } tickets;
} arch_spinlock_t;
static inline int arch_spin_is_locked(const arch_spinlock_t *lock)
{
    return __atomic_load_n(&lock->value, __ATOMIC_RELAXED) != 0U;
}
static inline int arch_spin_trylock(arch_spinlock_t *lock)
{
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(&lock->value, &expected, 1, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ? 1 : 0;
}
static inline void arch_spin_lock(arch_spinlock_t *lock)
{
    if (test_lock_role == 2) __atomic_store_n(&test_waiter_entered, 1, __ATOMIC_SEQ_CST);
    for (;;) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&lock->value, &expected, 1, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        sched_yield();
    }
}
static inline void arch_spin_unlock(arch_spinlock_t *lock)
{
    __atomic_store_n(&lock->value, 0, __ATOMIC_RELEASE);
}
#endif
