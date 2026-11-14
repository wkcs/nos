/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NUC472 片上 RTC 实时日历时钟驱动
 * 严格保护 SPR[0..1] 后备寄存器隔离，防篡改板级 boot_flag
 */

#include <stdint.h>
#include <stdbool.h>
#include <kernel/errno.h>
#include <soc/nuc472_soc.h>
#include <drivers/nuc472_periphs.h>

#define RTC_WAIT_TIMEOUT_CYCLES  400000UL

static int rtc_wait_access_enable(void)
{
    uint32_t timeout;

    if ((NUC472_RTC->RWEN & NUC472_RTC_RWEN_RWENF_Msk) == 0UL) {
        NUC472_RTC->RWEN = NUC472_RTC_WRITE_KEY;
        timeout = RTC_WAIT_TIMEOUT_CYCLES;
        while ((NUC472_RTC->RWEN & NUC472_RTC_RWEN_RWENF_Msk) == 0UL) {
            if (--timeout == 0UL) {
                return -ETIMEDOUT;
            }
        }
    }
    return 0;
}

int nuc472_rtc_init(void)
{
    uint32_t timeout;
    int ret;

    /* 开启 APBCLK0 RTC 外设时钟 */
    NUC472_CLK->APBCLK0 |= NUC472_CLK_APBCLK0_RTCCKEN;

    /* 若未处于活跃初始化状态则写 INIT 激活 RTC 32.768 kHz 域 */
    if ((NUC472_RTC->INIT & 0x1UL) == 0UL) {
        NUC472_RTC->INIT = NUC472_RTC_INIT_KEY;
        timeout = RTC_WAIT_TIMEOUT_CYCLES;
        while ((NUC472_RTC->INIT & 0x1UL) == 0UL) {
            if (--timeout == 0UL) {
                return -ETIMEDOUT;
            }
        }
    }

    /* 等待写入访问权限就绪 */
    ret = rtc_wait_access_enable();
    if (ret != 0) {
        return ret;
    }

    /* 配置为 24 小时制模式 (CLKFMT bit 0 = 1) */
    NUC472_RTC->CLKFMT |= 1UL;

    /* 注意：严格保持 SPR[0..23] 后备寄存器现状，绝不覆写或清零 boot_flag */
    return 0;
}

int nuc472_rtc_set_time(const struct nuc472_rtc_time *time)
{
    uint32_t cal;
    uint32_t tim;
    uint32_t year_offset;
    int ret;

    if (time == NULL) {
        return -EINVAL;
    }

    if (time->year < 2000U || time->year > 2099U ||
        time->month < 1U || time->month > 12U ||
        time->day < 1U || time->day > 31U ||
        time->hour > 23U || time->minute > 59U || time->second > 59U ||
        time->weekday > 6U) {
        return -EINVAL;
    }

    ret = rtc_wait_access_enable();
    if (ret != 0) {
        return ret;
    }

    year_offset = time->year - 2000U;

    /* BCD 编码日历寄存器 CAL */
    cal  = ((year_offset / 10U) & 0xFU) << 20;
    cal |= ((year_offset % 10U) & 0xFU) << 16;
    cal |= ((time->month / 10U) & 0x1U) << 12;
    cal |= ((time->month % 10U) & 0xFU) << 8;
    cal |= ((time->day / 10U) & 0x3U) << 4;
    cal |= ((time->day % 10U) & 0xFU);

    /* BCD 编码时间寄存器 TIME */
    tim  = ((time->hour / 10U) & 0x3U) << 20;
    tim |= ((time->hour % 10U) & 0xFU) << 16;
    tim |= ((time->minute / 10U) & 0x7U) << 12;
    tim |= ((time->minute % 10U) & 0xFU) << 8;
    tim |= ((time->second / 10U) & 0x7U) << 4;
    tim |= ((time->second % 10U) & 0xFU);

    NUC472_RTC->CAL = cal;
    NUC472_RTC->TIME = tim;
    NUC472_RTC->WEEKDAY = time->weekday & 0x7U;

    return 0;
}

int nuc472_rtc_get_time(struct nuc472_rtc_time *time)
{
    uint32_t cal;
    uint32_t tim;
    uint32_t wk;

    if (time == NULL) {
        return -EINVAL;
    }

    cal = NUC472_RTC->CAL;
    tim = NUC472_RTC->TIME;
    wk  = NUC472_RTC->WEEKDAY;

    /* BCD 解码日历 */
    time->year = 2000U + (((cal >> 20) & 0xFU) * 10U) + ((cal >> 16) & 0xFU);
    time->month = (((cal >> 12) & 0x1U) * 10U) + ((cal >> 8) & 0xFU);
    time->day = (((cal >> 4) & 0x3U) * 10U) + (cal & 0xFU);

    /* BCD 解码时间 */
    time->hour = (((tim >> 20) & 0x3U) * 10U) + ((tim >> 16) & 0xFU);
    time->minute = (((tim >> 12) & 0x7U) * 10U) + ((tim >> 8) & 0xFU);
    time->second = (((tim >> 4) & 0x7U) * 10U) + (tim & 0xFU);
    time->weekday = wk & 0x7U;

    return 0;
}
