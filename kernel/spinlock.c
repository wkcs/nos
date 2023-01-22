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

    lock->lock = false;
}

void spin_lock(spinlock_t *lock)
{
    if (lock == NULL) {
        pr_err("lock is NULL\r\n");
        return;
    }
    while (lock->lock) {}
    lock->irq_level = disable_irq_save();
    lock->lock = true;
}

void spin_unlock(spinlock_t *lock)
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
