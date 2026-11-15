/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NUC472 时钟初始化与系统频率计算 (12 MHz HXT -> 84 MHz PLL，失败自动回退 HIRC)
 */
#include <soc/nuc472_soc.h>

#ifndef NUC472_HOST_TEST
uint32_t SystemCoreClock = 22118400UL;
#else
static uint32_t g_test_core_clk = 22118400UL;
#define SystemCoreClock g_test_core_clk
#endif

static uint32_t g_hclk_hz = 22118400UL;
static uint32_t g_uart0_clk_hz = 22118400UL;

static bool clk_wait_ready(uint32_t status_mask)
{
    uint32_t timeout = 400000UL;
    while (timeout-- > 0UL) {
        if ((NUC472_CLK->STATUS & status_mask) == status_mask) {
            return true;
        }
    }
    return false;
}

uint32_t nuc472_clk_init_84mhz(void)
{
    uint32_t sys_hclk = NUC472_HIRC_HZ;

    nuc472_sys_unlock();

    /* 保持片内 HIRC 运行作为安全兜底时钟 */
    NUC472_CLK->PWRCTL |= NUC472_CLK_PWRCTL_HIRCEN;
    (void)clk_wait_ready(NUC472_CLK_STATUS_HIRCSTB);
    NUC472_CLK->CLKSEL0 = (NUC472_CLK->CLKSEL0 & ~NUC472_CLK_CLKSEL0_HCLKSEL_Msk) |
                          NUC472_CLK_CLKSEL0_HCLKSEL_HIRC;

    /* 启动外部 12 MHz HXT 与片内 10 kHz LIRC(用于 WDT)；LXT 仅在需要 RTC 精确日历时按键启用 */
    NUC472_CLK->PWRCTL |= NUC472_CLK_PWRCTL_HXTEN | NUC472_CLK_PWRCTL_LIRCEN;
    if (clk_wait_ready(NUC472_CLK_STATUS_HXTSTB)) {
        /* 84 MHz HCLK 需先关闭或配置 Flash 存取周期安全模式 (FTCTL.FOM = 000 为 >72 MHz) */
        NUC472_FMC->FTCTL &= ~NUC472_FMC_FTCTL_FOM_Msk;

        /* 配置锁相环 PLL 输出 84 MHz 时钟：12 MHz / 2 * 28 / 2 = 84 MHz (NUC472_CLK_PLLCTL_84MHZ_HXT = 0x0000401AUL) */
        NUC472_CLK->PLLCTL = NUC472_CLK_PLLCTL_84MHZ_HXT;
        if (clk_wait_ready(NUC472_CLK_STATUS_PLLSTB)) {
            NUC472_CLK->CLKDIV0 = (NUC472_CLK->CLKDIV0 &
                                   ~(NUC472_CLK_CLKDIV0_HCLKDIV_Msk | NUC472_CLK_CLKDIV0_UARTDIV_Msk));
            NUC472_CLK->CLKSEL0 = (NUC472_CLK->CLKSEL0 & ~NUC472_CLK_CLKSEL0_HCLKSEL_Msk) |
                                  NUC472_CLK_CLKSEL0_HCLKSEL_PLL;
            /* UART0 选择 12 MHz HXT 时钟源，115200 bps 误差低且不受 PLL 分频影响 */
            NUC472_CLK->CLKSEL1 = (NUC472_CLK->CLKSEL1 & ~NUC472_CLK_CLKSEL1_UARTSEL_Msk) |
                                  NUC472_CLK_CLKSEL1_UARTSEL_HXT;
            sys_hclk = NUC472_PLL_84MHZ_HZ;
            g_uart0_clk_hz = NUC472_HXT_HZ;
        }
    }

    if (sys_hclk != NUC472_PLL_84MHZ_HZ) {
        /* 回退 HIRC (22.1184 MHz)，清除分频恢复 1:1，UART0 同步切到 HIRC 避免波特率失真 */
        NUC472_CLK->CLKDIV0 = (NUC472_CLK->CLKDIV0 &
                               ~(NUC472_CLK_CLKDIV0_HCLKDIV_Msk | NUC472_CLK_CLKDIV0_UARTDIV_Msk));
        NUC472_CLK->CLKSEL0 = (NUC472_CLK->CLKSEL0 & ~NUC472_CLK_CLKSEL0_HCLKSEL_Msk) |
                              NUC472_CLK_CLKSEL0_HCLKSEL_HIRC;
        NUC472_CLK->CLKSEL1 = (NUC472_CLK->CLKSEL1 & ~NUC472_CLK_CLKSEL1_UARTSEL_Msk) |
                              NUC472_CLK_CLKSEL1_UARTSEL_HIRC;
        g_uart0_clk_hz = NUC472_HIRC_HZ;
    }

    nuc472_sys_lock();

    g_hclk_hz = sys_hclk;
    SystemCoreClock = sys_hclk;
    return sys_hclk;
}

uint32_t nuc472_clk_get_hclk_hz(void)
{
    return g_hclk_hz;
}

uint32_t nuc472_clk_get_pclk_hz(void)
{
    /* CLKSEL0.PCLKSEL 默认为 HCLK (1:1) */
    return (NUC472_CLK->CLKSEL0 & NUC472_CLK_CLKSEL0_PCLKSEL_Msk) ? (g_hclk_hz / 2UL) : g_hclk_hz;
}

uint32_t nuc472_clk_get_uart0_hz(void)
{
    return g_uart0_clk_hz;
}
