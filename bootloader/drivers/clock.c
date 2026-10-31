/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60-V2 Bootloader: 系统时钟、延时与 RTC 备份域管理实现
 */

#include "clock.h"
#include <stm32f4xx.h>

void delay_cycles(volatile uint32_t cycles)
{
    while (cycles-- > 0UL) {
        __asm__ volatile ("nop");
    }
}

void delay_us(uint32_t us)
{
    /* 84MHz 主频下，一个微秒大约 21 个循环 */
    delay_cycles(us * 21UL);
}

void delay_ms(uint32_t ms)
{
    while (ms-- > 0UL) {
        delay_us(1000UL);
    }
}

void clock_init_84mhz(void)
{
    uint32_t timeout = 0x5000UL;

    /* 启用 FPU (CP10, CP11 全访问) */
    SCB->CPACR |= ((3UL << 20U) | (3UL << 22U));

    /* 开启 HSI 内部时钟 */
    RCC->CR |= RCC_CR_HSION;
    RCC->CFGR = 0x00000000UL;
    RCC->CR &= ~(RCC_CR_HSEON | RCC_CR_CSSON | RCC_CR_PLLON);
    RCC->CIR = 0x00000000UL;

    /* 使能电源接口时钟并解锁 RTC 备份域 */
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    PWR->CR |= (PWR_CR_VOS | PWR_CR_DBP);

    /* 尝试启动 HSE 外部晶振 (8MHz) */
    RCC->CR |= RCC_CR_HSEON;
    while (((RCC->CR & RCC_CR_HSERDY) == 0UL) && (--timeout > 0UL)) {
    }

    /* 配置 Flash 预取、指令缓存、数据缓存与 2 等待周期 (84MHz) */
    FLASH->ACR = FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN | FLASH_ACR_LATENCY_2WS;

    /* APB1 预分频器 = 2 (42MHz), APB2 预分频器 = 1 (84MHz) */
    RCC->CFGR |= RCC_CFGR_PPRE1_DIV2;

    if (timeout > 0UL) {
        /* 8MHz HSE: M=8, N=336, P=4 (84MHz SYSCLK), Q=7 (48MHz USB) */
        RCC->PLLCFGR = 8UL | (336UL << 6U) | (1UL << 16U) | RCC_PLLCFGR_PLLSRC_HSE | (7UL << 24U);
    } else {
        /* 16MHz HSI: M=16, N=336, P=4 (84MHz SYSCLK), Q=7 (48MHz USB) */
        RCC->PLLCFGR = 16UL | (336UL << 6U) | (1UL << 16U) | RCC_PLLCFGR_PLLSRC_HSI | (7UL << 24U);
    }

    /* 使能 PLL */
    RCC->CR |= RCC_CR_PLLON;
    while ((RCC->CR & RCC_CR_PLLRDY) == 0UL) {
    }

    /* 切换系统主频到 PLL */
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {
    }
}

uint32_t backup_read_flag(void)
{
    return RTC->BKP0R;
}

void backup_write_flag(uint32_t val)
{
    PWR->CR |= PWR_CR_DBP;
    RTC->BKP0R = val;
}

void backup_clear_flag(void)
{
    PWR->CR |= PWR_CR_DBP;
    RTC->BKP0R = 0UL;
}

