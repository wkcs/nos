/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60-V2 Bootloader: 串口 1 (USART1) 驱动实现
 */

#include "uart.h"
#include <stm32f4xx.h>

void uart1_init(void)
{
    /* 使能 GPIOA 和 USART1 外设时钟 */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;

    /* PA9 (TX) & PA10 (RX) -> 复用功能 AF7 (USART1) */
    GPIOA->MODER = (GPIOA->MODER & ~((3UL << 18U) | (3UL << 20U))) |
                   (2UL << 18U) | (2UL << 20U);
    GPIOA->OSPEEDR |= (3UL << 18U) | (3UL << 20U);
    GPIOA->PUPDR = (GPIOA->PUPDR & ~((3UL << 18U) | (3UL << 20U))) |
                   (1UL << 18U) | (1UL << 20U);
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~((0xFUL << 4U) | (0xFUL << 8U))) |
                    (7UL << 4U) | (7UL << 8U);

    /* 84MHz APB2 下，115200 波特率对应的分频值为 0x02D9 (84000000 / (16 * 115200) = 45.5625) */
    USART1->BRR = 0x02D9UL;
    USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}

void uart_putc(char c)
{
    while ((USART1->SR & USART_SR_TXE) == 0UL) {
    }
    USART1->DR = (uint8_t)c;
}

void uart_puts(const char *s)
{
    while (*s != '\0') {
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s++);
    }
}

void uart_write(const uint8_t *data, uint16_t len)
{
    uint16_t i;
    for (i = 0U; i < len; i++) {
        uart_putc((char)data[i]);
    }
}

