/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_KERNEL_H__
#define __NOS_KERNEL_H__

#include <kernel/types.h>
#include <kernel/compiler.h>
#include <kernel/section.h>
#include <kernel/errno.h>
#include <kernel/minmax.h>
#include <kernel/printk.h>
#include <autocfg.h>

extern addr_t kernel_running;

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]) + __must_be_array(arr))

#define BUG_ON(ex) ({ \
    if (unlikely(ex)) { \
        pr_log(false, LOG_FATAL, "BUG_ON: %s:%d\r\n", __FILE__, __LINE__); \
    } \
})

#define IDEL_TASK_PID 1
#define CORE_TASK_PRIO 0
#define SYSTEM_TASK_PRIO 1
#define IDEL_TASK_PRIO (CONFIG_MAX_PRIORITY - 1)
#define MM_DEAMON_TASK_PRIO (CONFIG_MAX_PRIORITY - 2)

/**
 * is_power_of_2() - check if a value is a power of two
 * @n: the value to check
 *
 * Determine whether some value is a power of two, where zero is
 * *not* considered a power of two.
 * Return: true if @n is a power of 2, otherwise false.
 */
static inline __attribute__((const))
bool is_power_of_2(unsigned long n)
{
	return (n != 0 && ((n & (n - 1)) == 0));
}

#endif /* __NOS_KERNEL_H__ */
