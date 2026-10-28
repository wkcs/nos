/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * Author: Nick Hu <huqihan@live.com>
 */
#include <stdint.h>

#define REG32(addr) (*(volatile uint32_t *)(addr))
#define SYS_REGLCTL REG32(0x40000100UL)
#define CLK_PWRCTL REG32(0x40000200UL)
#define CLK_CLKSEL0 REG32(0x40000210UL)
#define CLK_CLKDIV0 REG32(0x40000220UL)
#define CLK_STATUS REG32(0x40000250UL)

uint32_t SystemCoreClock = 22118400U;

void SystemInit(void)
{
    SYS_REGLCTL = 0x59;
    SYS_REGLCTL = 0x16;
    SYS_REGLCTL = 0x88;
    CLK_PWRCTL |= 1U << 2;
    while ((CLK_STATUS & (1U << 4)) == 0) { }
    CLK_CLKDIV0 &= ~0xfU;
    CLK_CLKSEL0 = (CLK_CLKSEL0 & ~7U) | 7U;
    SYS_REGLCTL = 0;
#if defined(__VFP_FP__) && !defined(__SOFTFP__)
    REG32(0xe000ed88UL) |= 0xfU << 20;
    __asm__ volatile ("dsb\n isb" ::: "memory");
#endif
}
