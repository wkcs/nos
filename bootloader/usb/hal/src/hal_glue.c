/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60-V2 Bootloader: 精简 HAL 基础函数 (DWT 毫秒时基与延时)
 */

#include <stm32f4xx_hal.h>
#include "clock.h"

uint32_t HAL_GetTick(void)
{
    /* 使用 Cortex-M4 DWT 硬件时钟周期计数器 (84MHz: 84000 周期 = 1ms) */
    if ((CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) == 0UL) {
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    }
    return DWT->CYCCNT / 84000UL;
}

void HAL_Delay(uint32_t Delay)
{
    delay_ms(Delay);
}

uint32_t HAL_RCC_GetHCLKFreq(void)
{
    return 84000000UL;
}
