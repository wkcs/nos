/**
 * Copyright (C) 2026 Nick Hu
 *
 * Minimal host definitions for exercising the kernel formatter.
 */

#ifndef NOS_TEST_KERNEL_H
#define NOS_TEST_KERNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>

#define __printf(fmt, args) __attribute__((format(printf, fmt, args)))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define smp_wmb() __sync_synchronize()
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, val) ((x) = (val))
#define __acquire(lock) ((void)(lock))
#define __release(lock) ((void)(lock))
typedef unsigned int gfp_t;
extern uintptr_t kernel_running;
static inline bool is_power_of_2(unsigned long value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

#endif
