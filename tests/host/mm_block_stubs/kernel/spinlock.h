/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#pragma once
#include <pthread.h>
#include <sched.h>
typedef pthread_mutex_t spinlock_t;
#define SPINLOCK(n) spinlock_t n = PTHREAD_MUTEX_INITIALIZER
#define spin_lock_init(l) pthread_mutex_init((l), NULL)
#define spin_lock_irq(l) pthread_mutex_lock(l)
#define spin_unlock_irq(l) do { pthread_mutex_unlock(l); sched_yield(); } while (0)
