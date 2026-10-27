/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 */

#ifndef __X86_ASM_BARRIER_H__
#define __X86_ASM_BARRIER_H__

#include <kernel/kernel.h>

#define nop()   __asm__ __volatile__("nop" : : : "memory")
#define wfi()   __asm__ __volatile__("hlt" : : : "memory")

#define mb()        __asm__ __volatile__("mfence" : : : "memory")
#define rmb()       __asm__ __volatile__("lfence" : : : "memory")
#define wmb()       __asm__ __volatile__("sfence" : : : "memory")
#define smp_mb()    mb()
#define smp_rmb()   rmb()
#define smp_wmb()   wmb()

#endif /* __X86_ASM_BARRIER_H__ */
