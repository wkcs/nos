/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __ARM_ASM_TASK_H__
#define __ARM_ASM_TASK_H__

#include <kernel/kernel.h>

struct task_struct;

struct task_info {
    struct task_struct *task;
} __attribute__((aligned(4)));

static inline struct task_info *current_task_info(void) __attribute_const__;

static inline struct task_info *current_task_info(void)
{
    addr_t sp;
    asm ("mrs %0, psp\n" :"=r"(sp));
    return (struct task_info *)(sp & ~(CONFIG_PAGE_SIZE - 1));
}

#endif /* __ARM_ASM_TASK_H__ */
