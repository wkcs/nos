/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NUC472 片上 EMAC MDIO 链路探测驱动
 * 支持 CON1 接口上的 IP101GR 物理层 (PHY) 芯片识别与链路状态监测 (无 TCP/IP 负担)
 */

#include <stdint.h>
#include <stdbool.h>
#include <kernel/errno.h>
#include <soc/nuc472_soc.h>
#include <drivers/nuc472_periphs.h>

#define MDIO_TIMEOUT_CYCLES  100000UL

#ifdef NUC472_HOST_TEST
static bool g_sim_mdio_timeout = false;
int nuc472_emac_mdio_read_timeout_sim(uint32_t phy_addr, uint32_t reg_addr, uint16_t *val)
{
    g_sim_mdio_timeout = true;
    int ret = nuc472_emac_mdio_read(phy_addr, reg_addr, val);
    g_sim_mdio_timeout = false;
    return ret;
}
#endif

int nuc472_emac_phy_init(void)
{
    /* 解锁系统寄存器以配置 EMAC 时钟与引脚复用 */
    nuc472_sys_unlock();

    /* 开启 AHBCLK EMAC 外设时钟 */
    NUC472_CLK->AHBCLK |= NUC472_CLK_AHBCLK_EMACCKEN;

    /*
     * 配置 CLKDIV3 中的 EMACDIV 为 127U：
     * F_MDC = HCLK / (EMACDIV + 1) = 84 MHz / 128 = 656.25 kHz <= 2.5 MHz (符合 IEEE 802.3 规范)
     */
    NUC472_CLK->CLKDIV3 = (NUC472_CLK->CLKDIV3 & ~NUC472_CLK_CLKDIV3_EMACDIV_Msk) |
                          (127U << NUC472_CLK_CLKDIV3_EMACDIV_Pos);

    /* 配置 CON1 接口上的 RMII MDC (PB.14) 与 MDIO (PB.15) 多功能引脚复用 (功能号 6) */
    NUC472_SYS->GPB_MFPH = (NUC472_SYS->GPB_MFPH & ~NUC472_SYS_GPB_MFPH_RMII_MDIO_Msk) |
                           NUC472_SYS_GPB_MFPH_RMII_MDIO_Val;
    nuc472_sys_lock();

    return 0;
}

int nuc472_emac_mdio_read(uint32_t phy_addr, uint32_t reg_addr, uint16_t *val)
{
    uint32_t timeout;

    if (val == NULL || phy_addr > 31U || reg_addr > 31U) {
        return -EINVAL;
    }

    /* 设置 PHY 地址、寄存器地址并置位 MDCON 与 BUSY 启动 MDIO 读时钟周期 */
    NUC472_EMAC->MIIMCTL = (reg_addr & 0x1FU) |
                           (phy_addr << NUC472_EMAC_MIIMCTL_PHYADDR_Pos) |
                           NUC472_EMAC_MIIMCTL_MDCON |
                           NUC472_EMAC_MIIMCTL_BUSY;

#ifdef NUC472_HOST_TEST
    if (!g_sim_mdio_timeout) {
        /* 在主机回归测试环境下模拟 IP101GR 硬件返回数据 */
        static int s_bmsr_read_count = 0;
        if (phy_addr == 1U) {
            if (reg_addr == 2U) {
                NUC472_EMAC->MIIMDAT = 0x0243U; /* IP101GR PHY ID1 */
            } else if (reg_addr == 3U) {
                NUC472_EMAC->MIIMDAT = 0x0C54U; /* IP101GR PHY ID2 */
            } else if (reg_addr == 1U) {
                /* 模拟 Latch Low: 首次读出 0x7809 (Link Down), 再次读出 0x786D (Link Up) */
                NUC472_EMAC->MIIMDAT = (s_bmsr_read_count++ == 0) ? 0x7809U : 0x786DU;
            } else {
                NUC472_EMAC->MIIMDAT = 0x0000U;
            }
        }
        NUC472_EMAC->MIIMCTL &= ~NUC472_EMAC_MIIMCTL_BUSY;
    }
#endif

    /* 有界轮询等待 MIIMCTL.BUSY 清零 */
    timeout = MDIO_TIMEOUT_CYCLES;
    while ((NUC472_EMAC->MIIMCTL & NUC472_EMAC_MIIMCTL_BUSY) != 0UL) {
        if (--timeout == 0UL) {
            return -ETIMEDOUT;
        }
    }

    *val = (uint16_t)(NUC472_EMAC->MIIMDAT & 0xFFFFU);
    return 0;
}

int nuc472_emac_mdio_write(uint32_t phy_addr, uint32_t reg_addr, uint16_t val)
{
    uint32_t timeout;

    if (phy_addr > 31U || reg_addr > 31U) {
        return -EINVAL;
    }

    NUC472_EMAC->MIIMDAT = (uint32_t)val;

    /* 设置 PHY 地址、寄存器地址、置位 WRITE、MDCON 与 BUSY 启动写操作 */
    NUC472_EMAC->MIIMCTL = (reg_addr & 0x1FU) |
                           (phy_addr << NUC472_EMAC_MIIMCTL_PHYADDR_Pos) |
                           NUC472_EMAC_MIIMCTL_WRITE |
                           NUC472_EMAC_MIIMCTL_MDCON |
                           NUC472_EMAC_MIIMCTL_BUSY;

#ifdef NUC472_HOST_TEST
    NUC472_EMAC->MIIMCTL &= ~NUC472_EMAC_MIIMCTL_BUSY;
#endif

    timeout = MDIO_TIMEOUT_CYCLES;
    while ((NUC472_EMAC->MIIMCTL & NUC472_EMAC_MIIMCTL_BUSY) != 0UL) {
        if (--timeout == 0UL) {
            return -ETIMEDOUT;
        }
    }

    return 0;
}

/**
 * @brief 确保 EMAC 时钟与 MDIO 引脚复用已初始化（幂等）
 */
static void nuc472_emac_ensure_init(void)
{
    if ((NUC472_CLK->AHBCLK & NUC472_CLK_AHBCLK_EMACCKEN) == 0UL ||
        (NUC472_CLK->CLKDIV3 & NUC472_CLK_CLKDIV3_EMACDIV_Msk) == 0UL ||
        (NUC472_SYS->GPB_MFPH & NUC472_SYS_GPB_MFPH_RMII_MDIO_Msk) !=
        NUC472_SYS_GPB_MFPH_RMII_MDIO_Val) {
        (void)nuc472_emac_phy_init();
    }
}

int nuc472_emac_phy_probe(uint32_t phy_addr, struct nuc472_phy_info *info)
{
    uint16_t id1 = 0;
    uint16_t id2 = 0;
    uint16_t bmsr = 0;
    int ret;

    if (info == NULL || phy_addr > 31U) {
        return -EINVAL;
    }

    nuc472_emac_ensure_init();

    /* 读取 PHY ID 1 (寄存器 0x02) */
    ret = nuc472_emac_mdio_read(phy_addr, 2U, &id1);
    if (ret != 0) {
        return ret;
    }

    /* 总线浮空或无 PHY 芯片响应时，读数通常为 0xFFFF 或 0x0000 */
    if (id1 == 0xFFFFU || id1 == 0x0000U) {
        info->link_up = false;
        return -ENODEV;
    }

    /* 读取 PHY ID 2 (寄存器 0x03) */
    ret = nuc472_emac_mdio_read(phy_addr, 3U, &id2);
    if (ret != 0) {
        return ret;
    }

    /*
     * 读取基本状态寄存器 BMSR (寄存器 0x01)
     * IEEE 802.3 规范：BMSR 链路状态 (bit 2) 具备锁存低 (Latch Low) 粘滞特性，
     * 必须连续读取两次，丢弃第一次读出的历史粘滞值，以第二次读数反映当前链路实时状态。
     */
    ret = nuc472_emac_mdio_read(phy_addr, 1U, &bmsr);
    if (ret != 0) {
        return ret;
    }
    ret = nuc472_emac_mdio_read(phy_addr, 1U, &bmsr);
    if (ret != 0) {
        return ret;
    }

    info->phy_addr = phy_addr;
    info->phy_id1  = id1;
    info->phy_id2  = id2;
    info->bmsr     = bmsr;

    /* BMSR bit 2 为 Link Status (1 = 链路建立/网线已插入) */
    info->link_up  = ((bmsr & 0x0004U) != 0U);

    /* IP101GR 标称 PHY ID1=0x0243, ID2=0x0C54 (低 4 位为修订版本) */
    info->is_ip101gr = (id1 == 0x0243U && (id2 & 0xFFF0U) == 0x0C50U);

    return 0;
}
