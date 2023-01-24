/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __ARM_ASM_INIT_H__
#define __ARM_ASM_INIT_H__

#include <kernel/kernel.h>

void arch_init(void);

void asm_cpu_init(void);
void asm_cpu_delay_usec(uint32_t usec);
void asm_cpu_reboot(void);

#endif /* __ARM_ASM_INIT_H__ */
