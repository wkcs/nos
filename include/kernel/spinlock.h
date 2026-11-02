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
#include <kernel/sched.h>
#include <kernel/irq.h>
#include <asm/spinlock.h>

#ifndef ARCH_HAS_SPIN_TRYLOCK
static inline int arch_spin_is_locked(const arch_spinlock_t *lock)
{
    return lock->tickets.next != lock->tickets.owner;
}

static inline int arch_spin_trylock(arch_spinlock_t *lock)
{
    if (arch_spin_is_locked(lock)) {
        return 0;
    }
    lock->tickets.next++;
    return 1;
}
#endif

#if defined(__unix__) || defined(__APPLE__) || defined(_WIN32)
__attribute__((weak)) _Thread_local uint32_t __nos_spin_irq_depth;
__attribute__((weak)) _Thread_local addr_t __nos_spin_irq_flags;
static inline uint32_t *__nos_irq_depth_ptr(void)
{
    return &__nos_spin_irq_depth;
}
static inline addr_t *__nos_irq_flags_ptr(void)
{
    return &__nos_spin_irq_flags;
}
#else
#ifndef CONFIG_NR_CPUS
#define CONFIG_NR_CPUS 1
#endif
__attribute__((weak)) uint32_t __nos_spin_irq_depth[CONFIG_NR_CPUS];
__attribute__((weak)) addr_t __nos_spin_irq_flags[CONFIG_NR_CPUS];
static inline uint32_t __nos_spin_cpu_idx(void)
{
#if CONFIG_NR_CPUS > 1
    extern uint32_t hal_cpu_id(void);
    uint32_t cpu = hal_cpu_id();
    return (cpu < (uint32_t)CONFIG_NR_CPUS) ? cpu : 0U;
#else
    return 0U;
#endif
}
static inline uint32_t *__nos_irq_depth_ptr(void)
{
    return &__nos_spin_irq_depth[__nos_spin_cpu_idx()];
}
static inline addr_t *__nos_irq_flags_ptr(void)
{
    return &__nos_spin_irq_flags[__nos_spin_cpu_idx()];
}
#endif

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
  do { \
      addr_t __saved_irq_level = disable_irq_save(); \
      uint32_t *__depth_p = __nos_irq_depth_ptr(); \
      addr_t *__flags_p = __nos_irq_flags_ptr(); \
      if ((*__depth_p)++ == 0U) { \
          *__flags_p = __saved_irq_level; \
      } \
      __LOCK(&(lock)->rlock); \
      (lock)->irq_level = *__flags_p; \
  } while (0)
#define ___UNLOCK(rlock) \
  do { arch_spin_unlock(&(rlock)->raw_lock); __release(rlock); } while (0)
#define __UNLOCK(rlock) \
  do { ___UNLOCK(rlock); sch_unlock(); } while (0)
#define __UNLOCK_IRQ(lock) \
  do { \
      uint32_t *__depth_p = __nos_irq_depth_ptr(); \
      addr_t *__flags_p = __nos_irq_flags_ptr(); \
      addr_t __restore_irq_level = (lock)->irq_level; \
      bool __do_restore = true; \
      if (*__depth_p > 0U) { \
          if (--(*__depth_p) == 0U) { \
              __restore_irq_level = *__flags_p; \
          } else { \
              __do_restore = false; \
          } \
      } \
      __UNLOCK(&(lock)->rlock); \
      if (__do_restore) { \
          enable_irq_save(__restore_irq_level); \
      } \
  } while (0)

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

static inline int spin_is_locked(const spinlock_t *lock)
{
    return arch_spin_is_locked(&lock->rlock.raw_lock);
}

static inline int spin_trylock(spinlock_t *lock)
{
    sch_lock();
    if (arch_spin_trylock(&lock->rlock.raw_lock)) {
        __acquire(&lock->rlock);
#ifdef CONFIG_SPINLOCK_DEBUG
        lock->func = __func__;
        lock->line = __LINE__;
#endif
        return 1;
    }
    sch_unlock();
    return 0;
}

static inline int spin_trylock_irq(spinlock_t *lock)
{
    addr_t saved_irq = disable_irq_save();
    uint32_t *depth_p = __nos_irq_depth_ptr();
    addr_t *flags_p = __nos_irq_flags_ptr();

    sch_lock();
    if (arch_spin_trylock(&lock->rlock.raw_lock)) {
        __acquire(&lock->rlock);
        if ((*depth_p)++ == 0U) {
            *flags_p = saved_irq;
        }
        lock->irq_level = *flags_p;
#ifdef CONFIG_SPINLOCK_DEBUG
        lock->func = __func__;
        lock->line = __LINE__;
#endif
        return 1;
    }
    sch_unlock();
    if (*depth_p == 0U) {
        enable_irq_save(saved_irq);
    }
    return 0;
}

#define spin_lock_irqsave(lock, flags) \
do { \
    spin_lock_irq(lock); \
    (flags) = (lock)->irq_level; \
} while (0)

#define spin_unlock_irqrestore(lock, flags) \
do { \
    (void)(flags); \
    spin_unlock_irq(lock); \
} while (0)

#endif /* __NOS_SPINLOCK_H__ */
