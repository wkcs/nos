/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 */

#ifndef __RISCV_ASM_INIT_H__
#define __RISCV_ASM_INIT_H__

#include <kernel/kernel.h>

__init void arch_early_init(void);
__init void arch_init(void);

#endif /* __RISCV_ASM_INIT_H__ */
