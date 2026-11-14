/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60 Bootloader: 按键扫描实现（NK60-V1 直连 5x14 GPIO 矩阵 / NK60-V2 74HC595）
 */

#include "key.h"
#include "clock.h"
#include <stm32f4xx.h>

#if defined(BOARD_NK60_V1)

bool key_scan_recovery_pressed(void)
{
    uint32_t row2_idr;
    bool f_down;
    bool j_down;
    bool spc_down;

    /* 使能 GPIOA、GPIOB、GPIOC 时钟 */
    RCC->AHB1ENR |= (RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN | RCC_AHB1ENR_GPIOCEN);

    /*
     * 行输出 KEY_X1..KEY_X5（PC14, PA4, PA8, PA15, PB14）配置为推挽输出：
     * 扫描前将全部 5 条行线拉低，避免悬空或上电残留电平造成误检。
     */
    GPIOA->MODER = (GPIOA->MODER & ~((3UL << 8U) | (3UL << 16U) | (3UL << 30U))) |
                   (1UL << 8U) | (1UL << 16U) | (1UL << 30U);
    GPIOA->BSRR = (1UL << (4U + 16U)) | (1UL << (8U + 16U)) | (1UL << (15U + 16U));

    GPIOB->MODER = (GPIOB->MODER & ~(3UL << 28U)) | (1UL << 28U);
    GPIOB->BSRR = (1UL << (14U + 16U));

    /* PC14 作为行输出，PC0..PC13 配置为内部下拉输入 */
    GPIOC->MODER = (GPIOC->MODER & ~(0x0FFFFFFFUL | (3UL << 28U))) | (1UL << 28U);
    GPIOC->PUPDR = (GPIOC->PUPDR & ~0x0FFFFFFFUL) | 0x0AAAAAAAUL;
    GPIOC->BSRR = (1UL << (14U + 16U));
    delay_us(10U);

    /* 扫描 Row 2 (KEY_X3 = PA8)：检测 F (PC4, Col 4) 与 J (PC7, Col 7) */
    GPIOA->BSRR = (1UL << 8U);
    delay_us(10U);
    row2_idr = GPIOC->IDR;
    f_down = ((row2_idr & (1UL << 4U)) != 0UL);
    j_down = ((row2_idr & (1UL << 7U)) != 0UL);
    GPIOA->BSRR = (1UL << (8U + 16U));
    delay_us(20U);

    /* 扫描 Row 4 (KEY_X5 = PB14)：检测 Space (PC5, Col 5) */
    GPIOB->BSRR = (1UL << 14U);
    delay_us(10U);
    spc_down = ((GPIOC->IDR & (1UL << 5U)) != 0UL);
    GPIOB->BSRR = (1UL << (14U + 16U));
    delay_us(10U);

    return (spc_down && (f_down || j_down));
}

#else

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

#endif /* BOARD_NK60_V1 */

