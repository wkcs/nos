/* Copyright (C) 2026 NOS contributors. Privileged board clock mechanisms. */
#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define RCC 0x40023800u
#define GPIOA 0x40020000u
#define UART1 0x40011000u
#define TIM2 0x40000000u

static int wait_bits(uintptr_t address, uint32_t mask, uint32_t expected)
{
    for (uint32_t left = 8000000u; left; --left)
        if ((REG(address) & mask) == expected)
            return 0;
    return -1;
}

static void uart_init(uint32_t clock)
{
    REG(RCC + 0x30) |= 1u;
    REG(RCC + 0x44) |= 1u << 4;
    (void)REG(RCC + 0x44);
    REG(GPIOA) = (REG(GPIOA) & ~(15u << 18)) | (10u << 18);
    REG(GPIOA + 0x24) = (REG(GPIOA + 0x24) & ~(0xffu << 4)) | (0x77u << 4);
    REG(GPIOA + 0x0c) = (REG(GPIOA + 0x0c) & ~(3u << 20)) | (1u << 20);
    REG(UART1 + 0x0c) = 0;
    REG(UART1 + 8) = (clock + 57600u) / 115200u;
    REG(UART1 + 0x0c) = (1u << 13) | (1u << 3) | (1u << 2);
}

static void clock_fault(void)
{
    const char *s = "NOS nk60_v2: HSE/PLL clock failed; USB disabled\r\n";
    while (*s) {
        if (wait_bits(UART1, 1u << 7, 1u << 7))
            break;
        REG(UART1 + 4) = (uint8_t)*s++;
    }
    for (;;)
        __asm__ volatile("wfi");
}

void nk60_boot_init(void)
{
    /* Reset/ROM entry only; no inherited overclock/bootloader clock state. */
    REG(RCC) |= 1u;
    if (wait_bits(RCC, 2u, 2u))
        clock_fault();
    REG(RCC + 8) &= ~3u;
    if (wait_bits(RCC + 8, 12u, 0))
        clock_fault();
    REG(RCC) &= ~(1u << 24);
    if (wait_bits(RCC, 1u << 25, 0))
        clock_fault();
    uart_init(16000000u);
    REG(RCC + 0x40) |= 1u << 28;
    REG(0x40007000u) |= 1u << 14;
    REG(0x40023c00u) = (1u << 10) | (1u << 9) | (1u << 8) | 2u;
    REG(RCC) |= 1u << 16;
    if (wait_bits(RCC, 1u << 17, 1u << 17))
        clock_fault();
    /* HSE=8 MHz, VCO=336 MHz, SYSCLK=84 MHz, USB=48 MHz. */
    REG(RCC + 4) = 8u | (336u << 6) | (1u << 16) | (1u << 22) | (7u << 24);
    REG(RCC + 8) = (REG(RCC + 8) & ~0xfcf0u) | (4u << 10);
    REG(RCC) |= 1u << 24;
    if (wait_bits(RCC, 1u << 25, 1u << 25))
        clock_fault();
    REG(RCC + 8) = (REG(RCC + 8) & ~3u) | 2u;
    if (wait_bits(RCC + 8, 12u, 8u))
        clock_fault();
    uart_init(84000000u);
    REG(RCC + 0x40) |= 1u;
    (void)REG(RCC + 0x40);
    REG(TIM2) = 0;
    REG(TIM2 + 0x28) = 83u;
    REG(TIM2 + 0x2c) = UINT32_MAX;
    REG(TIM2 + 0x14) = 1u;
    REG(TIM2 + 0x10) = 0;
    REG(TIM2) = 1u;
    REG(0xe000ed08u) = 0x08000000u;
    __asm__ volatile("dsb\nisb" ::: "memory");
}
