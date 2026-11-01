/**
 * Copyright (C) 2023-2026 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_UNWIND_H__
#define __NOS_UNWIND_H__

#include <kernel/kernel.h>

struct task_struct;

#ifndef CONFIG_BACKTRACE_MAX_DEPTH
#define CONFIG_BACKTRACE_MAX_DEPTH 12
#endif

struct unwind_frame {
    uintptr_t pc;
    uintptr_t lr;
    uintptr_t sp;
    uintptr_t fp;
    uintptr_t stack_low;
    uintptr_t stack_high;
};

typedef int (*unwind_print_fn_t)(const char *fmt, ...);

/* Format "sym+0xoff/0xsize" or "0xaddr" into caller buffer (zero heap allocation) */
int kallsyms_sprint_symbol(char *buf, size_t size, unsigned long addr);

/* Unwind arbitrary task stack into caller-supplied PC array (returns frame count) */
size_t unwind_task_frames(struct task_struct *task,
                          uintptr_t *out_pcs,
                          size_t max_frames);

/* Unwind from explicit register/stack state into caller-supplied PC array */
size_t unwind_frames_from_state(const struct unwind_frame *init_frame,
                                uintptr_t text_start,
                                uintptr_t text_end,
                                bool thumb_check_bl,
                                uintptr_t *out_pcs,
                                size_t max_frames);

/* Stream backtrace of a specific task using print_fn (or pr_info if NULL) */
void dump_task_stack(struct task_struct *task, unwind_print_fn_t print_fn);

/* Stream backtrace from a trap/exception register frame */
void dump_trap_backtrace(uintptr_t sp,
                         uintptr_t pc,
                         uintptr_t lr,
                         uintptr_t fp,
                         uintptr_t stack_low,
                         uintptr_t stack_high,
                         unwind_print_fn_t print_fn);

#endif /* __NOS_UNWIND_H__ */
