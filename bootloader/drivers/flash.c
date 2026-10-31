/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60-V2 Bootloader: STM32F4 片上内部 Flash 驱动与内核跳转引导实现
 */

#include "flash.h"
#include <boot_proto.h>
#include <stm32f4xx.h>

#define FLASH_KEY1                  0x45670123UL
#define FLASH_KEY2                  0xCDEF89ABUL
#define FLASH_SR_ERR_MASK           0x000000F2UL

void flash_unlock(void)
{
    if ((FLASH->CR & FLASH_CR_LOCK) != 0UL) {
        FLASH->KEYR = FLASH_KEY1;
        FLASH->KEYR = FLASH_KEY2;
    }
}

void flash_lock_and_flush(void)
{
    /* 重置并刷新 Flash 指令与数据缓存 */
    FLASH->ACR &= ~(FLASH_ACR_ICEN | FLASH_ACR_DCEN);
    FLASH->ACR |= (FLASH_ACR_ICRST | FLASH_ACR_DCRST);
    FLASH->ACR &= ~(FLASH_ACR_ICRST | FLASH_ACR_DCRST);
    FLASH->ACR |= (FLASH_ACR_ICEN | FLASH_ACR_DCEN);
    FLASH->CR |= FLASH_CR_LOCK;
}

int flash_erase_sector(uint32_t sector_num)
{
    while ((FLASH->SR & FLASH_SR_BSY) != 0UL) {
    }
    FLASH->SR = FLASH_SR_ERR_MASK | FLASH_SR_EOP;
    FLASH->CR = FLASH_CR_PSIZE_1 | FLASH_CR_SER | ((sector_num & 0x0FUL) << 3U);
    FLASH->CR |= FLASH_CR_STRT;
    __asm__ volatile ("dsb" ::: "memory");
    while ((FLASH->SR & FLASH_SR_BSY) != 0UL) {
    }
    FLASH->CR &= ~(FLASH_CR_SER | FLASH_CR_SNB);
    return ((FLASH->SR & FLASH_SR_ERR_MASK) == 0UL) ? 0 : -1;
}

int flash_program_words(uint32_t dst_addr, const uint8_t *src, uint32_t len)
{
    uint32_t i;
    for (i = 0UL; i < len; i += 4UL) {
        uint32_t word = 0xFFFFFFFFUL;
        uint32_t rem = len - i;
        uint8_t *p_word = (uint8_t *)&word;
        uint32_t b;

        for (b = 0UL; b < ((rem >= 4UL) ? 4UL : rem); b++) {
            p_word[b] = src[i + b];
        }

        while ((FLASH->SR & FLASH_SR_BSY) != 0UL) {
        }
        FLASH->SR = FLASH_SR_ERR_MASK | FLASH_SR_EOP;
        FLASH->CR = FLASH_CR_PSIZE_1 | FLASH_CR_PG;
        (*(volatile uint32_t *)(uintptr_t)(dst_addr + i)) = word;
        __asm__ volatile ("dsb" ::: "memory");
        while ((FLASH->SR & FLASH_SR_BSY) != 0UL) {
        }
        FLASH->CR &= ~FLASH_CR_PG;
        if ((FLASH->SR & FLASH_SR_ERR_MASK) != 0UL) {
            return -1;
        }
    }
    return 0;
}

bool flash_is_kernel_valid(void)
{
    const uint32_t *vec = (const uint32_t *)(uintptr_t)NK60_KERNEL_FLASH_BASE;
    return nk60_validate_vector_table(vec, NK60_KERNEL_FLASH_BASE, NK60_INT_FLASH_END);
}

__attribute__((noreturn))
void flash_jump_to_app(uint32_t app_base)
{
    uint32_t app_msp = (*(volatile uint32_t *)(uintptr_t)app_base);
    uint32_t app_entry = (*(volatile uint32_t *)(uintptr_t)(app_base + 4UL));
    uint32_t i;

    /* 全局关闭中断 */
    __asm__ volatile ("cpsid i" ::: "memory");

    /* 停止 SysTick */
    SysTick->CTRL = 0UL;
    SysTick->LOAD = 0UL;
    SysTick->VAL = 0UL;

    /* 清除所有 NVIC 使能和挂起中断 */
    for (i = 0UL; i < 8UL; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFUL;
        NVIC->ICPR[i] = 0xFFFFFFFFUL;
    }

    /* 等待串口发送完毕 */
    while ((USART1->SR & USART_SR_TC) == 0UL) {
    }

    /* 重置外设复位寄存器 */
    RCC->APB1RSTR |= RCC_APB1RSTR_SPI2RST;
    RCC->APB1RSTR &= ~RCC_APB1RSTR_SPI2RST;

    /* 重设中断向量表 VTOR 并跳转 */
    SCB->VTOR = app_base;
    __asm__ volatile (
        "msr msp, %0\n\t"
        "dsb\n\t"
        "isb\n\t"
        "bx  %1\n\t"
        :
        : "r"(app_msp), "r"(app_entry)
        : "memory"
    );

    for (;;) {
    }
}

