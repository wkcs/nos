/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NUC472 片上 TIMER0 周期定时器与高精度微秒计数器驱动
 */

#include <stdint.h>
#include <stdbool.h>
#include <kernel/errno.h>
#include <soc/nuc472_soc.h>
#include <drivers/nuc472_periphs.h>

static volatile uint64_t g_timer0_ticks = 0;
static uint32_t g_timer0_freq_hz = 1000;

int nuc472_timer0_init(uint32_t freq_hz)
{
    uint32_t pclk;
    uint32_t prescale;
    uint32_t timer_clk;
    uint32_t cmp;

    if (freq_hz == 0U) {
        return -EINVAL;
    }

    g_timer0_freq_hz = freq_hz;

    /* 开启 APBCLK0 TIMER0 外设时钟 */
    NUC472_CLK->APBCLK0 |= NUC472_CLK_APBCLK0_TMR0CKEN;

    /* 选择 TIMER0 时钟源为 PCLK (84 MHz) */
    NUC472_CLK->CLKSEL1 = (NUC472_CLK->CLKSEL1 & ~NUC472_CLK_CLKSEL1_TMR0SEL_Msk) |
                          NUC472_CLK_CLKSEL1_TMR0SEL_PCLK;

    pclk = nuc472_clk_get_pclk_hz();
    if (pclk == 0U) {
        pclk = 84000000UL;
    }

    /* 将定时器计数时钟预分频为 1 MHz (每计数 1 us) */
    prescale = (pclk / 1000000UL);
    if (prescale > 0U) {
        prescale -= 1U;
    }
    if (prescale > 255U) {
        prescale = 255U;
    }

    timer_clk = pclk / (prescale + 1U);
    cmp = timer_clk / freq_hz;
    if (cmp < 2U) {
        cmp = 2U;
    }

    /* 设置周期模式、使能超时中断并装载分频值 */
    NUC472_TMR0->CTL = NUC472_TIMER_CTL_MODE_PERIODIC |
                       NUC472_TIMER_CTL_INTEN |
                       (prescale & 0xFFU);
    NUC472_TMR0->CMP = cmp;

    /* 软件复位定时器计数器 */
    NUC472_TMR0->CTL |= NUC472_TIMER_CTL_RSTCNT;

    return 0;
}

int nuc472_timer0_start(void)
{
    NUC472_TMR0->CTL |= NUC472_TIMER_CTL_CNTEN;
    return 0;
}

int nuc472_timer0_stop(void)
{
    NUC472_TMR0->CTL &= ~NUC472_TIMER_CTL_CNTEN;
    return 0;
}

uint64_t nuc472_timer0_get_ticks(void)
{
    return g_timer0_ticks;
}

uint64_t nuc472_timer0_get_us(void)
{
    uint64_t ticks = g_timer0_ticks;
    uint32_t cnt = NUC472_TMR0->CNT & 0x00FFFFFFUL;
    return (ticks * 1000000ULL) / (uint64_t)g_timer0_freq_hz + (uint64_t)cnt;
}

void nuc472_timer0_irq_handler(void)
{
    if ((NUC472_TMR0->INTSTS & NUC472_TIMER_INTSTS_TIF) != 0UL) {
        /* 写 1 清除中断标志 */
        NUC472_TMR0->INTSTS = NUC472_TIMER_INTSTS_TIF;
        g_timer0_ticks++;
    }
}
