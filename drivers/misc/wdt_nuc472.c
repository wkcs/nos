/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NUC472 片上看门狗定时器 (WDT) 驱动 (基于 10 kHz LIRC)
 */

#include <stdint.h>
#include <stdbool.h>
#include <kernel/errno.h>
#include <soc/nuc472_soc.h>
#include <drivers/nuc472_periphs.h>

#define WDT_TIMEOUT_CYCLES  200000UL

int nuc472_wdt_init(void)
{
    uint32_t timeout;

    /* 启动片内 10 kHz 低速内部振荡器 (LIRC) */
    NUC472_CLK->PWRCTL |= NUC472_CLK_PWRCTL_LIRCEN;

    /* 等待 LIRC 时钟稳定 */
    timeout = WDT_TIMEOUT_CYCLES;
    while ((NUC472_CLK->STATUS & NUC472_CLK_STATUS_LIRCSTB) == 0UL) {
        if (--timeout == 0UL) {
            return -ETIMEDOUT;
        }
    }

    /* 开启 APBCLK0 WDT 外设时钟 */
    NUC472_CLK->APBCLK0 |= NUC472_CLK_APBCLK0_WDTCKEN;

    /* 切换 WDT 时钟源为 LIRC */
    NUC472_CLK->CLKSEL1 = (NUC472_CLK->CLKSEL1 & ~NUC472_CLK_CLKSEL1_WDTSEL_Msk) |
                          NUC472_CLK_CLKSEL1_WDTSEL_LIRC;

    /* 默认初始化必须保持看门狗处于关闭状态 */
    nuc472_sys_unlock();
    NUC472_WDT->CTL = 0UL;
    nuc472_sys_lock();

    return 0;
}

int nuc472_wdt_start(uint32_t timeout_interval, bool enable_reset)
{
    uint32_t ctl;

    if (timeout_interval > 7U) {
        return -EINVAL;
    }

    ctl = ((timeout_interval & 0x7U) << NUC472_WDT_CTL_TOUTSEL_Pos) |
          NUC472_WDT_CTL_WDTEN |
          NUC472_WDT_CTL_RSTCNT;

    if (enable_reset) {
        ctl |= NUC472_WDT_CTL_RSTEN;
    }

    nuc472_sys_unlock();
    NUC472_WDT->CTL = ctl;
    nuc472_sys_lock();

    return 0;
}

int nuc472_wdt_feed(void)
{
    nuc472_sys_unlock();
    /* 置位 RSTCNT 复位看门狗计数器，避开写 1 清零位 */
    NUC472_WDT->CTL = (NUC472_WDT->CTL & ~0x1EUL) | NUC472_WDT_CTL_RSTCNT;
    nuc472_sys_lock();
    return 0;
}

int nuc472_wdt_stop(void)
{
    nuc472_sys_unlock();
    NUC472_WDT->CTL = 0UL;
    nuc472_sys_lock();
    return 0;
}
