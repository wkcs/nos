/** Copyright (C) 2026 Nick Hu */
#ifndef NOS_TEST_SPINLOCK_H
#define NOS_TEST_SPINLOCK_H
#include <stdint.h>
typedef int spinlock_t;
#define SPINLOCK(name) spinlock_t name
#define spin_lock_init(lock) (*(lock) = 0)
#define spin_lock(lock) ((void)(lock))
#define spin_unlock(lock) ((void)(lock))
#define spin_lock_irq(lock) ((void)(lock))
#define spin_unlock_irq(lock) ((void)(lock))
#endif
