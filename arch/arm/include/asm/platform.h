/**
 * Copyright (C) 2023-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __ARM_ASM_PLATFORM_H__
#define __ARM_ASM_PLATFORM_H__

#include <kernel/types.h>

typedef enum IRQn {
    NonMaskableInt_IRQn = -14,
    MemoryManagement_IRQn = -12,
    BusFault_IRQn = -11,
    UsageFault_IRQn = -10,
    SVCall_IRQn = -5,
    DebugMonitor_IRQn = -4,
    PendSV_IRQn = -2,
    SysTick_IRQn = -1,
} IRQn_Type;

#if defined(__ARM_ARCH_7M__)
#include <core_cm3.h>
#elif defined(__ARM_ARCH_7EM__)
#if defined(__VFP_FP__) && !defined(__SOFTFP__)
#define __FPU_PRESENT 1U
#endif
#include <core_cm4.h>
#else
#error "Unsupported ARM Cortex-M platform"
#endif

extern uint32_t SystemCoreClock;

#endif /* __ARM_ASM_PLATFORM_H__ */
