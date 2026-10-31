/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60-V2 Bootloader: 74HC595 键盘扫描实现
 */

#include "key.h"
#include "clock.h"
#include <stm32f4xx.h>

#define NK60_RAW_BITS               72U
#define NK60_RAW_BIT_SPACE          51U
#define NK60_RAW_BIT_F              68U
#define NK60_RAW_BIT_J              71U

bool key_scan_recovery_pressed(void)
{
    uint32_t i;
    bool f_down = false;
    bool j_down = false;
    bool spc_down = false;

    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;

    /* PB12(CS), PB13(CLK), PB15(DATA_OUT) 输出; PB14(DATA_IN) 输入 */
    GPIOB->MODER = (GPIOB->MODER & ~((3UL << 24U) | (3UL << 26U) | (3UL << 28U) | (3UL << 30U))) |
                   (1UL << 24U) | (1UL << 26U) | (1UL << 30U);
    GPIOB->PUPDR = (GPIOB->PUPDR & ~(3UL << 28U)) | (1UL << 28U); /* PB14 上拉 */
    GPIOB->BSRR = (1UL << (12U + 16U)) | (1UL << (13U + 16U)) | (1UL << (15U + 16U));
    delay_us(10U);

    for (i = 0UL; i < NK60_RAW_BITS; i++) {
        if (i == 0UL) {
            GPIOB->BSRR = (1UL << (15U + 16U));
        } else {
            GPIOB->BSRR = (1UL << 15U);
        }
        delay_us(1U);
        GPIOB->BSRR = (1UL << 13U);
        delay_us(1U);
        GPIOB->BSRR = (1UL << (13U + 16U));
        GPIOB->BSRR = (1UL << 12U);
        delay_us(1U);
        GPIOB->BSRR = (1UL << (12U + 16U));
        delay_us(10U);

        if ((GPIOB->IDR & (1UL << 14U)) == 0UL) {
            if (i == NK60_RAW_BIT_F) {
                f_down = true;
            } else if (i == NK60_RAW_BIT_J) {
                j_down = true;
            } else if (i == NK60_RAW_BIT_SPACE) {
                spc_down = true;
            }
        }
    }

    return (spc_down && (f_down || j_down));
}

