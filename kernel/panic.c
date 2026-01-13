/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/cpu.h>
#include <kernel/irq.h>
#include <kernel/kernel.h>
#include <kernel/printk.h>
#include <kernel/task.h>
#include <stdarg.h>

void dump_stack(void) { arch_dump_stack(); }

void panic(const char *fmt, ...) {
  va_list args;

  disable_irq_save();
  pr_fatal("Kernel panic - not syncing: ");
  va_start(args, fmt);
  vprintk(fmt, args);
  va_end(args);
  pr_fatal("\r\n");

  dump_stack();

  /* 触发一个硬故障，进入cpu_hard_fault进行处理 */
  __asm__ volatile("udf #0");

  while (1)
    ;
}
