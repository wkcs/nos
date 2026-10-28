/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#ifndef NOS_TASK_STACK_LAYOUT_H
#define NOS_TASK_STACK_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

static inline uintptr_t task_stack_base(uintptr_t allocation, uint32_t size,
                                        bool grows_up)
{
    const uintptr_t mask = (uintptr_t)15U;
    if (grows_up) return (allocation + mask) & ~mask;
    return ((allocation + size + mask) & ~mask) - size;
}

#endif
