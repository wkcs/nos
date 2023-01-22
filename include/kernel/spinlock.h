/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_SPINLOCK_H__
#define __NOS_SPINLOCK_H__

#include <kernel/types.h>

typedef struct spinlock {
    bool lock;
    addr_t irq_level;
} spinlock_t;

void spin_lock_init(spinlock_t *lock);
void spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);

#define SPINLOCK(name) \
spinlock_t name = {    \
    .lock = false,     \
}

#endif /* __NOS_SPINLOCK_H__ */
