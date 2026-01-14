/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_KERNEL_H__
#define __NOS_KERNEL_H__

#include <autocfg.h>
#include <kernel/compiler.h>
#include <kernel/errno.h>
#include <kernel/minmax.h>
#include <kernel/printk.h>
#include <kernel/section.h>
#include <kernel/types.h>

extern addr_t kernel_running;

#ifndef CONFIG_MAX_PRIORITY
#define CONFIG_MAX_PRIORITY 32
#endif

#ifndef CONFIG_LOG_FIFO_BUF_SIZE
#define CONFIG_LOG_FIFO_BUF_SIZE 4096
#endif

#ifndef CONFIG_PAGE_SIZE
#define CONFIG_PAGE_SIZE 4096
#endif

#ifndef CONFIG_VERSION_CODE
#define CONFIG_VERSION_CODE 0x00010000
#endif

#ifndef CONFIG_CPU_TYPE
#define CONFIG_CPU_TYPE "ARM"
#endif

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]) + __must_be_array(arr))

void panic(const char *fmt, ...);
void dump_stack(void);

#define BUG_ON(ex)                                                             \
  ({                                                                           \
    if (unlikely(ex)) {                                                        \
      panic("BUG_ON: %s:%d", __FILE__, __LINE__);                              \
    }                                                                          \
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
static inline __attribute__((const)) bool is_power_of_2(unsigned long n) {
  return (n != 0 && ((n & (n - 1)) == 0));
}

#endif /* __NOS_KERNEL_H__ */
