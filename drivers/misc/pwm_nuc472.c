/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NUC472 片上 PWM0 脉宽调制波形发生器驱动 (PA.5/PA.6 输出)
 */

#include <stdint.h>
#include <stdbool.h>
#include <kernel/errno.h>
#include <soc/nuc472_soc.h>
#include <drivers/nuc472_periphs.h>

#define PWM_TIMEOUT_CYCLES  100000UL

int nuc472_pwm_init(uint32_t ch)
{
    if (ch >= 6U) {
        return -EINVAL;
    }
    if (ch >= 2U) {
        return -ENOTSUP;
    }

    nuc472_sys_unlock();

    /* 开启 APBCLK1 PWM0CH01 外设时钟 */
    NUC472_CLK->APBCLK1 |= NUC472_CLK_APBCLK1_PWM0CH01CKEN;

    /* 选择 PWM0 时钟源为 PCLK (84 MHz) */
    NUC472_CLK->CLKSEL2 = (NUC472_CLK->CLKSEL2 & ~NUC472_CLK_CLKSEL2_PWM0CH01_Msk) |
                          NUC472_CLK_CLKSEL2_PWM0CH01_PCLK;

    /* 配置板载 PA.5 (CH0) 与 PA.6 (CH1) 的多功能引脚复用为 PWM (功能号 2) */
    if (ch == 0U) {
        nuc472_mfp_set(&NUC472_SYS->GPA_MFPL,
                       NUC472_SYS_MFPL_Msk(5U),
                       NUC472_SYS_MFPL_Val(5U, 2U));
    } else if (ch == 1U) {
        nuc472_mfp_set(&NUC472_SYS->GPA_MFPL,
                       NUC472_SYS_MFPL_Msk(6U),
                       NUC472_SYS_MFPL_Val(6U, 2U));
    }

    nuc472_sys_lock();
    return 0;
}

int nuc472_pwm_config(uint32_t ch, uint32_t freq_hz, uint32_t duty_percent)
{
    uint32_t pwm_clk;
    uint32_t divider;
    uint32_t prescale;
    uint32_t cnr;
    uint32_t i;
    uint32_t reg_div;
    uint32_t timeout;
    uint32_t shift_psc;
    uint32_t shift_div;

    if (ch >= 6U || freq_hz == 0U || duty_percent > 100U) {
        return -EINVAL;
    }
    if (ch >= 2U) {
        return -ENOTSUP;
    }

    /* 默认 PWM 输入时钟为 PCLK (84 MHz) */
    pwm_clk = nuc472_clk_get_pclk_hz();
    if (pwm_clk == 0U) {
        pwm_clk = 84000000UL;
    }

    /* 计算最匹配的目标分频器与周期值 */
    divider = 1U;
    prescale = 2U;
    cnr = 0xFFFFU;

    for (; divider <= 16U; divider <<= 1U) {
        i = (pwm_clk / freq_hz) / divider;
        if (i > (0x10000UL * 0x100UL)) {
            continue;
        }

        prescale = (i + 0xFFFFUL) / 0x10000UL;
        if (prescale < 2U) {
            prescale = 2U;
        }

        i /= prescale;
        if (i <= 0x10000UL) {
            cnr = (i == 1U) ? 1U : i;
            break;
        }
    }

    /* 映射硬件分频寄存器编码 */
    if (divider == 1U) {
        reg_div = 4U;
    } else if (divider == 2U) {
        reg_div = 0U;
    } else if (divider == 4U) {
        reg_div = 1U;
    } else if (divider == 8U) {
        reg_div = 2U;
    } else {
        reg_div = 3U;
    }

    /* 轮询等待 SBS 寄存器空闲以安全写入时钟分频 */
    timeout = PWM_TIMEOUT_CYCLES;
    while ((NUC472_PWM0->SBS[ch] & 1U) != 0U) {
        if (--timeout == 0UL) {
            return -ETIMEDOUT;
        }
    }

    /* 每 2 个通道共享一个 Prescaler (8 位) */
    shift_psc = (ch >> 1U) * 8U;
    NUC472_PWM0->CLKPSC = (NUC472_PWM0->CLKPSC & ~(0xFFUL << shift_psc)) |
                          (((prescale - 1U) & 0xFFUL) << shift_psc);

    /* 每个通道拥有专属的 4 位 Divider */
    shift_div = ch * 4U;
    NUC472_PWM0->CLKDIV = (NUC472_PWM0->CLKDIV & ~(0xFUL << shift_div)) |
                          ((reg_div & 0xFUL) << shift_div);

    /* 设置为边沿对齐自动重载计数模式 */
    NUC472_PWM0->CTL |= NUC472_PWM_CTL_CNTMODE_AUTO(ch);

    /* 设置周期值与比较值 (占空比) */
    NUC472_PWM0->PERIOD[ch] = cnr - 1U;
    if (duty_percent == 0U) {
        NUC472_PWM0->CMPDAT[ch] = 0U;
    } else {
        NUC472_PWM0->CMPDAT[ch] = (duty_percent * cnr) / 100U - 1U;
    }

    /* 使能通道计数与引脚输出 */
    NUC472_PWM0->CNTEN |= NUC472_PWM_CNTEN_CH(ch);
    NUC472_PWM0->POEN  |= NUC472_PWM_POEN_CH(ch);

    return 0;
}

int nuc472_pwm_start(uint32_t ch)
{
    if (ch >= 6U) {
        return -EINVAL;
    }
    if (ch >= 2U) {
        return -ENOTSUP;
    }

    NUC472_PWM0->CNTEN |= NUC472_PWM_CNTEN_CH(ch);
    NUC472_PWM0->POEN  |= NUC472_PWM_POEN_CH(ch);
    return 0;
}

int nuc472_pwm_stop(uint32_t ch)
{
    if (ch >= 6U) {
        return -EINVAL;
    }
    if (ch >= 2U) {
        return -ENOTSUP;
    }

    NUC472_PWM0->CNTEN &= ~NUC472_PWM_CNTEN_CH(ch);
    NUC472_PWM0->POEN  &= ~NUC472_PWM_POEN_CH(ch);
    return 0;
}
