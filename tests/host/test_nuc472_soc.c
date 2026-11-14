/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NUC472 SoC 寄存器布局偏移与时钟切换/回退主机单元测试
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NUC472_HOST_TEST 1
#include "../../soc/nuc472/include/soc/nuc472_regs.h"
#include "../../soc/nuc472/include/soc/nuc472_periph.h"
#include "../../soc/nuc472/include/soc/nuc472_soc.h"

/* 依据 NUC472 TRM 与官方 NUC472_442.h 验证全部外设关键寄存器字节偏移 */
_Static_assert(offsetof(NUC472_SYS_Type, PORCTL) == 0x24U, "SYS->PORCTL 偏移必须为 0x24");
_Static_assert(offsetof(NUC472_SYS_Type, VREFCTL) == 0x28U, "SYS->VREFCTL 偏移必须为 0x28");
_Static_assert(offsetof(NUC472_SYS_Type, USBPHY) == 0x2CU, "SYS->USBPHY 偏移必须为 0x2C");
_Static_assert(offsetof(NUC472_SYS_Type, GPA_MFPL) == 0x30U, "SYS->GPA_MFPL 偏移必须为 0x30");
_Static_assert(offsetof(NUC472_SYS_Type, GPB_MFPH) == 0x3CU, "SYS->GPB_MFPH 偏移必须为 0x3C");
_Static_assert(offsetof(NUC472_SYS_Type, GPH_MFPL) == 0x68U, "SYS->GPH_MFPL 偏移必须为 0x68");
_Static_assert(offsetof(NUC472_SYS_Type, REGLCTL) == 0x100U, "SYS->REGLCTL 偏移必须为 0x100");
_Static_assert(sizeof(NUC472_SYS_Type) == 0x104U, "sizeof(NUC472_SYS_Type) 必须为 0x104");

_Static_assert(offsetof(NUC472_CLK_Type, PWRCTL) == 0x00U, "CLK->PWRCTL 偏移必须为 0x00");
_Static_assert(offsetof(NUC472_CLK_Type, AHBCLK) == 0x04U, "CLK->AHBCLK 偏移必须为 0x04");
_Static_assert(offsetof(NUC472_CLK_Type, APBCLK0) == 0x08U, "CLK->APBCLK0 偏移必须为 0x08");
_Static_assert(offsetof(NUC472_CLK_Type, APBCLK1) == 0x0CU, "CLK->APBCLK1 偏移必须为 0x0C");
_Static_assert(offsetof(NUC472_CLK_Type, CLKSEL0) == 0x10U, "CLK->CLKSEL0 偏移必须为 0x10");
_Static_assert(offsetof(NUC472_CLK_Type, CLKSEL1) == 0x14U, "CLK->CLKSEL1 偏移必须为 0x14");
_Static_assert(offsetof(NUC472_CLK_Type, CLKDIV0) == 0x20U, "CLK->CLKDIV0 偏移必须为 0x20");
_Static_assert(offsetof(NUC472_CLK_Type, PLLCTL) == 0x40U, "CLK->PLLCTL 偏移必须为 0x40");
_Static_assert(offsetof(NUC472_CLK_Type, STATUS) == 0x50U, "CLK->STATUS 偏移必须为 0x50");
_Static_assert(sizeof(NUC472_CLK_Type) == 0x74U, "sizeof(NUC472_CLK_Type) 必须为 0x74");

_Static_assert(offsetof(NUC472_GPIO_Type, SLEWCTL) == 0x28U, "GPIO->SLEWCTL 偏移必须为 0x28");
_Static_assert(sizeof(NUC472_GPIO_Type) == 0x2CU, "sizeof(NUC472_GPIO_Type) 必须为 0x2C");

_Static_assert(offsetof(NUC472_FMC_Type, FTCTL) == 0x18U, "FMC->FTCTL 偏移必须为 0x18");
_Static_assert(offsetof(NUC472_FMC_Type, ISPSTS) == 0x40U, "FMC->ISPSTS 偏移必须为 0x40");
_Static_assert(offsetof(NUC472_FMC_Type, MPSTS) == 0xC0U, "FMC->MPSTS 偏移必须为 0xC0");
_Static_assert(sizeof(NUC472_FMC_Type) == 0xC8U, "sizeof(NUC472_FMC_Type) 必须为 0xC8");

_Static_assert(offsetof(NUC472_RTC_Type, RWEN) == 0x04U, "RTC->RWEN 偏移必须为 0x04");
_Static_assert(offsetof(NUC472_RTC_Type, CLKFMT) == 0x14U, "RTC->CLKFMT 偏移必须为 0x14");
_Static_assert(offsetof(NUC472_RTC_Type, SPRCTL) == 0x3CU, "RTC->SPRCTL 偏移必须为 0x3C");
_Static_assert(offsetof(NUC472_RTC_Type, SPR) == 0x40U, "RTC->SPR 偏移必须为 0x40");
_Static_assert(offsetof(NUC472_RTC_Type, TAMSK) == 0x140U, "RTC->TAMSK 偏移必须为 0x140");
_Static_assert(sizeof(NUC472_RTC_Type) == 0x148U, "sizeof(NUC472_RTC_Type) 必须为 0x148");

_Static_assert(offsetof(NUC472_PWM_Type, CNTEN) == 0x0CU, "PWM->CNTEN 偏移必须为 0x0C");
_Static_assert(offsetof(NUC472_PWM_Type, PERIOD) == 0x10U, "PWM->PERIOD 偏移必须为 0x10");
_Static_assert(offsetof(NUC472_PWM_Type, CMPDAT) == 0x28U, "PWM->CMPDAT 偏移必须为 0x28");
_Static_assert(offsetof(NUC472_PWM_Type, POEN) == 0x7CU, "PWM->POEN 偏移必须为 0x7C");
_Static_assert(sizeof(NUC472_PWM_Type) == 0xF8U, "sizeof(NUC472_PWM_Type) 必须为 0xF8");

_Static_assert(offsetof(NUC472_EADC_Type, AD0DAT0) == 0x00U, "EADC->AD0DAT0 偏移必须为 0x00");
_Static_assert(offsetof(NUC472_EADC_Type, CTL) == 0x40U, "EADC->CTL 偏移必须为 0x40");
_Static_assert(offsetof(NUC472_EADC_Type, SWTRG) == 0x48U, "EADC->SWTRG 偏移必须为 0x48");
_Static_assert(offsetof(NUC472_EADC_Type, AD0SPCTL0) == 0x58U, "EADC->AD0SPCTL0 偏移必须为 0x58");
_Static_assert(offsetof(NUC472_EADC_Type, STATUS0) == 0xB0U, "EADC->STATUS0 偏移必须为 0xB0");
_Static_assert(offsetof(NUC472_EADC_Type, STATUS1) == 0xB4U, "EADC->STATUS1 偏移必须为 0xB4");
_Static_assert(sizeof(NUC472_EADC_Type) == 0x164U, "sizeof(NUC472_EADC_Type) 必须为 0x164");

_Static_assert(offsetof(NUC472_USBD_Type, OPER) == 0x18U, "USBD->OPER 偏移必须为 0x18");
_Static_assert(offsetof(NUC472_USBD_Type, SETUP1_0) == 0x44U, "USBD->SETUP1_0 偏移必须为 0x44");
_Static_assert(offsetof(NUC472_USBD_Type, CEPBUFSTART) == 0x54U, "USBD->CEPBUFSTART 偏移必须为 0x54");
_Static_assert(offsetof(NUC472_USBD_Type, EP) == 0x64U, "USBD->EP[0] 偏移必须为 0x64");
_Static_assert(offsetof(NUC472_USBD_Type, DMAADDR) == 0x700U, "USBD->DMAADDR 偏移必须为 0x700");
_Static_assert(offsetof(NUC472_USBD_Type, PHYCTL) == 0x704U, "USBD->PHYCTL 偏移必须为 0x704");
_Static_assert(sizeof(NUC472_USBD_EP_Type) == 0x28U, "USBD_EP 步长必须为 0x28");
_Static_assert(sizeof(NUC472_USBD_Type) == 0x708U, "sizeof(NUC472_USBD_Type) 必须为 0x708");

_Static_assert(offsetof(NUC472_EMAC_Type, CTL) == 0x90U, "EMAC->CTL 偏移必须为 0x90");
_Static_assert(offsetof(NUC472_EMAC_Type, MIIMCTL) == 0x98U, "EMAC->MIIMCTL 偏移必须为 0x98");
_Static_assert(sizeof(NUC472_EMAC_Type) == 0x130U, "sizeof(NUC472_EMAC_Type) 必须为 0x130");

_Static_assert(offsetof(NUC472_UART_Type, FIFOSTS) == 0x18U, "UART->FIFOSTS 偏移必须为 0x18");
_Static_assert(offsetof(NUC472_UART_Type, BAUD) == 0x24U, "UART->BAUD 偏移必须为 0x24");
_Static_assert(offsetof(NUC472_SPI_Type, CLKDIV) == 0x04U, "SPI->CLKDIV 偏移必须为 0x04");
_Static_assert(offsetof(NUC472_SPI_Type, STATUS) == 0x14U, "SPI->STATUS 偏移必须为 0x14");
_Static_assert(offsetof(NUC472_I2C_Type, STATUS) == 0x0CU, "I2C->STATUS 偏移必须为 0x0C");

static NUC472_SYS_Type g_stub_sys;
static NUC472_CLK_Type g_stub_clk;
static NUC472_FMC_Type g_stub_fmc;

NUC472_SYS_Type *nuc472_test_sys(void)
{
    return &g_stub_sys;
}

NUC472_CLK_Type *nuc472_test_clk(void)
{
    return &g_stub_clk;
}

NUC472_FMC_Type *nuc472_test_fmc(void)
{
    return &g_stub_fmc;
}

#include "../../soc/nuc472/clk.c"
#include "../../soc/nuc472/sys_mfp.c"

int main(void)
{
    uint32_t hclk;

    /* 测试场景 1: REGLCTL 已可解锁且 HXT/PLL 就绪 -> 切换到 84 MHz PLL */
    memset(&g_stub_sys, 0, sizeof(g_stub_sys));
    memset(&g_stub_clk, 0, sizeof(g_stub_clk));
    memset(&g_stub_fmc, 0, sizeof(g_stub_fmc));
    g_stub_sys.REGLCTL = 1U; /* 主机桩模拟写解锁密钥后芯片置位 REGLCTL=1 */
    g_stub_clk.STATUS = NUC472_CLK_STATUS_HXTSTB | NUC472_CLK_STATUS_HIRCSTB | NUC472_CLK_STATUS_PLLSTB;
    hclk = nuc472_clk_init_84mhz();
    assert(hclk == 84000000UL);
    assert((g_stub_clk.CLKSEL0 & NUC472_CLK_CLKSEL0_HCLKSEL_Msk) == NUC472_CLK_CLKSEL0_HCLKSEL_PLL);
    assert(g_stub_clk.PLLCTL == NUC472_CLK_PLLCTL_84MHZ_HXT);

    /* 测试场景 2: 无外部晶振 (HXTSTB=0) -> 超时安全回退 HIRC (22.1184 MHz) */
    memset(&g_stub_clk, 0, sizeof(g_stub_clk));
    g_stub_sys.REGLCTL = 1U;
    g_stub_clk.STATUS = NUC472_CLK_STATUS_HIRCSTB;
    hclk = nuc472_clk_init_84mhz();
    assert(hclk == 22118400UL);
    assert((g_stub_clk.CLKSEL0 & NUC472_CLK_CLKSEL0_HCLKSEL_Msk) == NUC472_CLK_CLKSEL0_HCLKSEL_HIRC);

    /* 测试场景 3: MFP 寄存器写入不破坏同寄存器其他引脚 */
    nuc472_mfp_set(&g_stub_sys.GPG_MFPL, NUC472_SYS_MFPL_Msk(1U), (1UL << 4));
    nuc472_mfp_set(&g_stub_sys.GPG_MFPL, NUC472_SYS_MFPL_Msk(2U), (1UL << 8));
    assert((g_stub_sys.GPG_MFPL & 0xFF0U) == 0x110U);

    printf("NUC472 SoC layout & clock tests passed\n");
    return 0;
}
