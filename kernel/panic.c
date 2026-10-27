/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/cpu.h>
#include <kernel/console.h>
#include <kernel/irq.h>
#include <kernel/kernel.h>
#include <kernel/printk.h>
#include <kernel/task.h>
#include <stdarg.h>

void dump_stack(void) { arch_dump_stack(); }

void panic(const char *fmt, ...) {
  va_list args;

  disable_irq_save();
  (void)console_panic_begin();
  kernel_log_panic_flush();
  console_write("Kernel panic - not syncing: ", 28);
  va_start(args, fmt);
  vprintk(fmt, args);
  va_end(args);
  console_write("\r\n", 2);

  dump_stack();

  /* 触发一个硬故障，进入cpu_hard_fault进行处理 */
  __builtin_trap();

  while (1)
    ;
}
