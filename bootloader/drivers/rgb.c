/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60-V2 Bootloader: WS2812 RGB 状态指示灯驱动实现
 */

#include "rgb.h"
#include "clock.h"
#include <stm32f4xx.h>

void rgb_init(void)
{
    /* 使能 GPIOB, GPIOC 与 SPI2 时钟 */
    RCC->AHB1ENR |= (RCC_AHB1ENR_GPIOBEN | RCC_AHB1ENR_GPIOCEN);
    RCC->APB1ENR |= RCC_APB1ENR_SPI2EN;

    /* PB0 配置为推挽输出 (灯珠电源使能/片选控制) */
    GPIOB->MODER = (GPIOB->MODER & ~(3UL << 0U)) | (1UL << 0U);
    GPIOB->BSRR = (1UL << 16U); /* 低电平复位 */

    /* PC3 配置为 SPI2_MOSI (AF5) */
    GPIOC->MODER = (GPIOC->MODER & ~(3UL << 6U)) | (2UL << 6U);
    GPIOC->OSPEEDR |= (3UL << 6U);
    GPIOC->AFR[0] = (GPIOC->AFR[0] & ~(0xFUL << 12U)) | (5UL << 12U);

    /* SPI2 主机模式，双线只发，波特率分频 = 8 (42MHz / 8 = 5.25MHz), 对应 WS2812 码元时序 */
    SPI2->CR1 = SPI_CR1_BIDIMODE | SPI_CR1_BIDIOE | SPI_CR1_SSM | SPI_CR1_SSI |
                (2UL << 3U) | SPI_CR1_MSTR | SPI_CR1_CPHA | SPI_CR1_CPOL | SPI_CR1_SPE;
}

static void rgb_spi_send_byte(uint8_t val)
{
    int i;
    for (i = 0; i < 8; i++) {
        uint8_t wire = ((val << i) & 0x80U) ? 0xF0U : 0xC0U;
        while ((SPI2->SR & SPI_SR_TXE) == 0UL) {
        }
        SPI2->DR = wire;
    }
}

void rgb_set_solid(uint8_t r, uint8_t g, uint8_t b)
{
    int i;
    GPIOB->BSRR = (1UL << 0U);
    delay_us(200U);
    for (i = 0; i < 61; i++) {
        rgb_spi_send_byte(g);
        rgb_spi_send_byte(r);
        rgb_spi_send_byte(b);
    }
    while ((SPI2->SR & SPI_SR_BSY) != 0UL) {
    }
    delay_us(100U);
}

