/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NUC472 片上 EADC 增强型模数转换器驱动
 * 支持片内带隙基准 (VBG) 与晶圆温度传感器 (VTEMP) 采样换算
 */

#include <stdint.h>
#include <stdbool.h>
#include <kernel/errno.h>
#include <soc/nuc472_soc.h>
#include <drivers/nuc472_periphs.h>

#define EADC_TIMEOUT_CYCLES  100000UL

int nuc472_eadc_init(void)
{
    nuc472_sys_unlock();

    /* 开启 APBCLK1 EADC 外设时钟 */
    NUC472_CLK->APBCLK1 |= NUC472_CLK_APBCLK1_EADCCKEN;

    /*
     * 配置 EADC 采样时钟源与分频：
     * 若外部高速晶振 HXT 就绪，选用 12 MHz HXT 且分频比为 1 (ADCDIV=0，时钟 12 MHz <= 16 MHz)；
     * 若外部晶振失效或未起振，自适应安全回退到片内 HIRC (22.1184 MHz)，分频比为 2 (ADCDIV=1，时钟 11.06 MHz <= 16 MHz)。
     */
    if ((NUC472_CLK->STATUS & NUC472_CLK_STATUS_HXTSTB) != 0UL) {
        NUC472_CLK->CLKSEL1 = (NUC472_CLK->CLKSEL1 & ~NUC472_CLK_CLKSEL1_ADCSEL_Msk) |
                              NUC472_CLK_CLKSEL1_ADCSEL_HXT;
        NUC472_CLK->CLKDIV0 = (NUC472_CLK->CLKDIV0 & ~NUC472_CLK_CLKDIV0_ADCDIV_Msk) |
                              (0U << NUC472_CLK_CLKDIV0_ADCDIV_Pos);
    } else {
        NUC472_CLK->CLKSEL1 = (NUC472_CLK->CLKSEL1 & ~NUC472_CLK_CLKSEL1_ADCSEL_Msk) |
                              NUC472_CLK_CLKSEL1_ADCSEL_HIRC;
        NUC472_CLK->CLKDIV0 = (NUC472_CLK->CLKDIV0 & ~NUC472_CLK_CLKDIV0_ADCDIV_Msk) |
                              (1U << NUC472_CLK_CLKDIV0_ADCDIV_Pos);
    }

    /* 配置 EADC 模块基准源为 AVDD，并切换 ADC IP 模式为 EADC */
    NUC472_SYS->VREFCTL = (NUC472_SYS->VREFCTL & ~NUC472_SYS_VREFCTL_VREFCTL_Msk) |
                          NUC472_SYS_VREFCTL_VREF_AVDD |
                          NUC472_SYS_VREFCTL_ADCMODESEL_Msk;

    /* 使能内部温度传感器供电偏置 */
    NUC472_SYS->TEMPCTL |= NUC472_SYS_TEMPCTL_VTEMPEN_Msk;

    nuc472_sys_lock();

    /* 开启 EADC 模拟核心电源 */
    NUC472_EADC->CTL |= NUC472_EADC_CTL_ADCEN;

    return 0;
}

int nuc472_eadc_sample_raw(uint32_t ch, uint32_t *raw_val)
{
    uint32_t timeout;

    if (raw_val == NULL) {
        return -EINVAL;
    }

    /* 配置采样逻辑模块 0 (AD0SPCTL0)：指定通道并选用软件触发 (TRGSEL=0) */
    NUC472_EADC->AD0SPCTL0 = (ch & 0x1FU);

    /* 软件触发采样模块 0 启动一次 A/D 转换 */
    NUC472_EADC->SWTRG = 1U;

    /* 有界轮询等待采样完成 (STATUS0 bit 0 有效标志置位) */
    timeout = EADC_TIMEOUT_CYCLES;
    while ((NUC472_EADC->STATUS0 & 1U) == 0U) {
        if (--timeout == 0UL) {
            return -ETIMEDOUT;
        }
    }

    /* 读取 12 位有效转换结果 (0 ~ 4095) */
    *raw_val = NUC472_EADC->AD0DAT0 & NUC472_EADC_DAT_RESULT_Msk;
    return 0;
}

int nuc472_eadc_read_vbg_mv(uint32_t *mv)
{
    uint32_t raw;
    int ret;

    if (mv == NULL) {
        return -EINVAL;
    }

    ret = nuc472_eadc_sample_raw(NUC472_EADC_CH_VBG, &raw);
    if (ret != 0) {
        return ret;
    }

    /* 基于 AVDD 3.3V (3300 mV) 满量程 4095 计算毫伏电压 (带四舍五入) */
    *mv = (raw * 3300UL + 2047UL) / 4095UL;
    return 0;
}

int nuc472_eadc_read_temp_celsius(int32_t *temp_c)
{
    uint32_t raw;
    uint32_t mv;
    int ret;

    if (temp_c == NULL) {
        return -EINVAL;
    }

    ret = nuc472_eadc_sample_raw(NUC472_EADC_CH_VTEMP, &raw);
    if (ret != 0) {
        return ret;
    }

    mv = (raw * 3300UL + 2047UL) / 4095UL;

    /*
     * NUC472 片内晶圆温度传感器典型特性曲线：
     * 25 °C 标称输出电压为 700 mV，温漂斜率约为 -1.8 mV/°C
     * 换算公式: T = 25 + (700 - Vtemp_mv) / 1.8
     */
    *temp_c = (int32_t)25 + (((int32_t)700 - (int32_t)mv) * 10) / 18;
    return 0;
}
