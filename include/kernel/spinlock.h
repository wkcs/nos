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

typedef struct spinlock {
    bool lock;
    addr_t irq_level;
#ifdef CONFIG_SPINLOCK_DEBUG
    const char *func;
    u32 line;
#endif
} spinlock_t;

void spin_lock_init(spinlock_t *lock);
void __spin_lock(spinlock_t *lock);
void __spin_unlock(spinlock_t *lock);
#ifdef CONFIG_SPINLOCK_DEBUG
#define spin_lock(lock) \
do { \
    __spin_lock(lock); \
    (lock)->func = __func__; \
    (lock)->line = __LINE__; \
} while (0)
#define spin_unlock(lock) \
do { \
    (lock)->func = NULL; \
    (lock)->line = 0; \
    __spin_unlock(lock); \
} while (0)
#else /* CONFIG_SPINLOCK_DEBUG */
#define spin_lock(lock) __spin_lock(lock);
#define spin_unlock(lock) __spin_unlock(lock);
#endif

#define SPINLOCK(name) \
spinlock_t name = {    \
    .lock = false,     \
}

#endif /* __NOS_SPINLOCK_H__ */
