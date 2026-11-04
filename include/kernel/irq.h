/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_IRQ_H__
#define __NOS_IRQ_H__

#include <stdbool.h>
#include <nos/hal_arch.h>

addr_t disable_irq_save();
void enable_irq_save(addr_t level);

static inline bool in_interrupt(void)
{
    return hal_in_irq();
}

#endif /* __NOS_IRQ_H__ */
