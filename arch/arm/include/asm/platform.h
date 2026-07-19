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
#ifndef __NVIC_PRIO_BITS
#define __NVIC_PRIO_BITS 4U
#endif
#include <core_cm3.h>
#elif defined(__ARM_ARCH_7EM__)
#ifndef __CM4_REV
#define __CM4_REV 0x0001U
#endif
#ifndef __MPU_PRESENT
#define __MPU_PRESENT 1U
#endif
#ifndef __NVIC_PRIO_BITS
#define __NVIC_PRIO_BITS 4U
#endif
#ifndef __Vendor_SysTickConfig
#define __Vendor_SysTickConfig 0U
#endif
#if defined(__VFP_FP__) && !defined(__SOFTFP__)
#ifndef __FPU_PRESENT
#define __FPU_PRESENT 1U
#endif
#endif
#include <core_cm4.h>
#else
#error "Unsupported ARM Cortex-M platform"
#endif

extern uint32_t SystemCoreClock;

#endif /* __ARM_ASM_PLATFORM_H__ */
