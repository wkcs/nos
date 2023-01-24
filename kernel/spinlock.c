/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/spinlock.h>
#include <kernel/kernel.h>
#include <kernel/printk.h>
#include <kernel/irq.h>

void spin_lock_init(spinlock_t *lock)
{
    if (lock == NULL) {
        pr_err("lock is NULL\r\n");
        return;
    }

#ifdef CONFIG_SPINLOCK_DEBUG
    lock->func = __func__;
    lock->line = __LINE__;
#endif

    lock->lock = false;
}

void __spin_lock(spinlock_t *lock)
{
    if (lock == NULL) {
        pr_err("lock is NULL\r\n");
        return;
    }

#ifdef CONFIG_SPINLOCK_DEBUG
    if (lock->lock) {
        pr_err("lock in %s[%u]\r\n", lock->func, lock->line);
    }
#endif
    while (lock->lock) {}
    lock->irq_level = disable_irq_save();
    lock->lock = true;
}

void __spin_unlock(spinlock_t *lock)
{
    if (lock == NULL) {
        pr_err("lock is NULL\r\n");
        return;
    }

    if (lock->lock) {
        lock->lock = false;
        enable_irq_save(lock->irq_level);
    }
}
