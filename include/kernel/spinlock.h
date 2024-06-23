/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_SPINLOCK_H__
#define __NOS_SPINLOCK_H__

#include <kernel/kernel.h>
#include <kernel/sch.h>
#include <kernel/irq.h>
#include <asm/spinlock.h>

typedef struct raw_spinlock {
    arch_spinlock_t raw_lock;
} raw_spinlock_t;

typedef struct spinlock {
    struct raw_spinlock rlock;
    addr_t irq_level;
#ifdef CONFIG_SPINLOCK_DEBUG
    const char *func;
    u32 line;
#endif
} spinlock_t;

static inline void raw_spin_lock_init(raw_spinlock_t *lock)
{
    lock->raw_lock.tickets.next = 0;
    lock->raw_lock.tickets.owner = 0;
}

#ifdef CONFIG_SPINLOCK_DEBUG
#define spin_lock_init(_lock)            \
do {                                     \
    raw_spin_lock_init(&(_lock)->rlock); \
    (_lock)->func = NULL;                  \
    (_lock)->line = 0;                     \
} while (0)
#else
#define spin_lock_init(_lock)            \
do {                                     \
    raw_spin_lock_init(&(_lock)->rlock); \
} while (0)
#endif

#define ___LOCK(rlock) \
  do { __acquire(rlock); arch_spin_lock(&(rlock)->raw_lock); } while (0)
#define __LOCK(rlock) \
  do { sch_lock(); ___LOCK(rlock); } while (0)
#define __LOCK_IRQ(lock) \
  do { (lock)->irq_level = disable_irq_save(); __LOCK(&(lock)->rlock); } while (0)
#define ___UNLOCK(rlock) \
  do { arch_spin_unlock(&(rlock)->raw_lock); __release(rlock); } while (0)
#define __UNLOCK(rlock) \
  do { ___UNLOCK(rlock); sch_unlock(); } while (0)
#define __UNLOCK_IRQ(lock) \
  do { __UNLOCK(&(lock)->rlock); enable_irq_save((lock)->irq_level); } while (0)

#define _raw_spin_lock(rlock) __LOCK(rlock)
#define raw_spin_lock(rlock) _raw_spin_lock(rlock)
#define _raw_spin_lock_irq(lock) __LOCK_IRQ(lock)
#define raw_spin_lock_irq(lock) _raw_spin_lock_irq(lock)

#define _raw_spin_unlock(rlock) __UNLOCK(rlock)
#define raw_spin_unlock(rlock) _raw_spin_unlock(rlock)
#define _raw_spin_unlock_irq(lock) __UNLOCK_IRQ(lock)
#define raw_spin_unlock_irq(lock) _raw_spin_unlock_irq(lock)

#ifdef CONFIG_SPINLOCK_DEBUG
#define SPINLOCK(name)      \
spinlock_t name = {         \
    .rlock = {              \
        .raw_lock = {       \
            .tickets = {    \
                .owner = 0, \
                .next = 0,  \
            },              \
        },                  \
    },                      \
    .func = NULL,           \
    .line = 0,              \
}
#else
#define SPINLOCK(name)      \
spinlock_t name = {         \
    .rlock = {              \
        .raw_lock = {       \
            .tickets = {    \
                .owner = 0, \
                .next = 0,  \
            },              \
        },                  \
    },                      \
}
#endif

#ifdef CONFIG_SPINLOCK_DEBUG
#define spin_lock(lock) \
do { \
    raw_spin_lock(&(lock)->rlock); \
    (lock)->func = __func__; \
    (lock)->line = __LINE__; \
} while (0)

#define spin_lock_irq(lock) \
do { \
    raw_spin_lock_irq(lock); \
    (lock)->func = __func__; \
    (lock)->line = __LINE__; \
} while (0)

#define spin_unlock(lock) \
do { \
    (lock)->func = NULL; \
    (lock)->line = 0; \
    raw_spin_unlock(&(lock)->rlock); \
} while (0)

#define spin_unlock_irq(lock) \
do { \
    (lock)->func = NULL; \
    (lock)->line = 0; \
    raw_spin_unlock_irq(lock); \
} while (0)
#else
static inline void spin_lock(spinlock_t *lock)
{
    raw_spin_lock(&lock->rlock);
}

static inline void spin_unlock(spinlock_t *lock)
{
    raw_spin_unlock(&lock->rlock);
}

static inline void spin_lock_irq(spinlock_t *lock)
{
    raw_spin_lock_irq(lock);
}

static inline void spin_unlock_irq(spinlock_t *lock)
{
    raw_spin_unlock_irq(lock);
}
#endif

#endif /* __NOS_SPINLOCK_H__ */
