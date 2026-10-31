/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * Host test stub for kernel/mutex.h.
 */

#ifndef __NOS_MUTEX_H__
#define __NOS_MUTEX_H__

struct mutex {
    int locked;
};

static inline int mutex_init(struct mutex *lock)
{
    if (lock) {
        lock->locked = 0;
    }
    return 0;
}

static inline void mutex_lock(struct mutex *lock)
{
    if (lock) {
        lock->locked = 1;
    }
}

static inline void mutex_unlock(struct mutex *lock)
{
    if (lock) {
        lock->locked = 0;
    }
}

#endif /* __NOS_MUTEX_H__ */

