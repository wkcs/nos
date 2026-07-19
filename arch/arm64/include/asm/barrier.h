/**
 * Copyright (C) 2023-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __ARM64_ASM_BARRIER_H__
#define __ARM64_ASM_BARRIER_H__

#include <kernel/kernel.h>

#define nop() __asm__ __volatile__("nop" : : : "memory")

#define sev()	__asm__ __volatile__("sev" : : : "memory")
#define wfe()	__asm__ __volatile__("wfe" : : : "memory")
#define wfi()	__asm__ __volatile__("wfi" : : : "memory")

#define isb(option) __asm__ __volatile__("isb " #option : : : "memory")
#define dsb(option) __asm__ __volatile__("dsb " #option : : : "memory")
#define dmb(option) __asm__ __volatile__("dmb " #option : : : "memory")

#ifndef CONFIG_SMP
#define mb()		barrier()
#define rmb()		barrier()
#define wmb()		barrier()
#define smp_mb()	barrier()
#define smp_rmb()	barrier()
#define smp_wmb()	barrier()
#else
#define mb()		dsb(sy)
#define rmb()		dsb(ld)
#define wmb()		dsb(st)
#define smp_mb()	dmb(ish)
#define smp_rmb()	dmb(ishld)
#define smp_wmb()	dmb(ishst)
#endif

#endif /* __ARM64_ASM_BARRIER_H__ */
