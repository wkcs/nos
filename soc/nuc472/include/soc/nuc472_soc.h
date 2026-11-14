/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NUC472 SoC 底层系统控制、时钟与引脚复用接口
 */
#ifndef __NOS_SOC_NUC472_SOC_H__
#define __NOS_SOC_NUC472_SOC_H__

#include <stdint.h>
#include <stdbool.h>
#include "nuc472_regs.h"
#include "nuc472_periph.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef NUC472_HOST_TEST
#define NUC472_SYS        ((NUC472_SYS_Type *)NUC472_SYS_BASE)
#define NUC472_CLK        ((NUC472_CLK_Type *)NUC472_CLK_BASE)
#define NUC472_FMC        ((NUC472_FMC_Type *)NUC472_FMC_BASE)
#define NUC472_RTC        ((NUC472_RTC_Type *)NUC472_RTC_BASE)
#define NUC472_WDT        ((NUC472_WDT_Type *)NUC472_WDT_BASE)
#define NUC472_EADC       ((NUC472_EADC_Type *)NUC472_EADC_BASE)
#define NUC472_TMR0       ((NUC472_TIMER_Type *)NUC472_TIMER0_BASE)
#define NUC472_PWM0       ((NUC472_PWM_Type *)NUC472_PWM0_BASE)
#define NUC472_SPI0       ((NUC472_SPI_Type *)NUC472_SPI0_BASE)
#define NUC472_UART0      ((NUC472_UART_Type *)NUC472_UART0_BASE)
#define NUC472_I2C3       ((NUC472_I2C_Type *)NUC472_I2C3_BASE)
#define NUC472_USBD       ((NUC472_USBD_Type *)NUC472_USBD_BASE)
#define NUC472_EMAC       ((NUC472_EMAC_Type *)NUC472_EMAC_BASE)
#define NUC472_GPIO(port) ((NUC472_GPIO_Type *)NUC472_GPIO_PORT(port))
#else
NUC472_SYS_Type *nuc472_test_sys(void);
NUC472_CLK_Type *nuc472_test_clk(void);
NUC472_FMC_Type *nuc472_test_fmc(void);
#define NUC472_SYS        (nuc472_test_sys())
#define NUC472_CLK        (nuc472_test_clk())
#define NUC472_FMC        (nuc472_test_fmc())
#endif

extern uint32_t SystemCoreClock;

/* 系统寄存器写保护解锁/上锁 (NUC472 TRM: 顺序写入 0x59, 0x16, 0x88) */
static inline void nuc472_sys_unlock(void)
{
    while (NUC472_SYS->REGLCTL == 0U) {
        NUC472_SYS->REGLCTL = 0x59UL;
        NUC472_SYS->REGLCTL = 0x16UL;
        NUC472_SYS->REGLCTL = 0x88UL;
    }
}

static inline void nuc472_sys_lock(void)
{
    NUC472_SYS->REGLCTL = 0U;
}

uint32_t nuc472_clk_init_84mhz(void);
uint32_t nuc472_clk_get_hclk_hz(void);
uint32_t nuc472_clk_get_pclk_hz(void);
uint32_t nuc472_clk_get_uart0_hz(void);
void nuc472_mfp_set(volatile uint32_t *reg, uint32_t mask, uint32_t val);
void nuc472_mfp_config_board_defaults(void);
int  nuc472_fmc_read_uid(uint32_t uid[3]);

#ifndef NUC472_HOST_TEST
static inline void nuc472_nvic_enable_irq(uint32_t irqn)
{
    volatile uint32_t *iser = (volatile uint32_t *)0xE000E100UL;
    iser[irqn >> 5] = (1UL << (irqn & 0x1FU));
}

static inline void nuc472_nvic_disable_irq(uint32_t irqn)
{
    volatile uint32_t *icer = (volatile uint32_t *)0xE000E180UL;
    icer[irqn >> 5] = (1UL << (irqn & 0x1FU));
}

static inline void nuc472_nvic_clear_pending_irq(uint32_t irqn)
{
    volatile uint32_t *icpr = (volatile uint32_t *)0xE000E280UL;
    icpr[irqn >> 5] = (1UL << (irqn & 0x1FU));
}

static inline void nuc472_nvic_set_priority(uint32_t irqn, uint8_t prio_4bit)
{
    volatile uint8_t *ip = (volatile uint8_t *)0xE000E400UL;
    ip[irqn] = (uint8_t)((prio_4bit & 0x0FU) << 4);
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* __NOS_SOC_NUC472_SOC_H__ */
