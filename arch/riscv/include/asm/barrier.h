/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 */

#ifndef __RISCV_ASM_BARRIER_H__
#define __RISCV_ASM_BARRIER_H__

#include <kernel/kernel.h>

#define nop()   __asm__ __volatile__("nop" : : : "memory")
#define wfi()   __asm__ __volatile__("wfi" : : : "memory")

#define mb()        __asm__ __volatile__("fence iorw, iorw" : : : "memory")
#define rmb()       __asm__ __volatile__("fence ir, ir" : : : "memory")
#define wmb()       __asm__ __volatile__("fence ow, ow" : : : "memory")
#define smp_mb()    __asm__ __volatile__("fence rw, rw" : : : "memory")
#define smp_rmb()   __asm__ __volatile__("fence r, r" : : : "memory")
#define smp_wmb()   __asm__ __volatile__("fence w, w" : : : "memory")

#endif /* __RISCV_ASM_BARRIER_H__ */
