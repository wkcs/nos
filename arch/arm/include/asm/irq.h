/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __ARM_ASM_IRQ_H__
#define __ARM_ASM_IRQ_H__

#include <kernel/kernel.h>

addr_t asm_disable_irq_save();
void asm_enable_irq_save(addr_t level);

#endif /* __ARM_ASM_IRQ_H__ */
