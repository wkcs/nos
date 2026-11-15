/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NUC472 硬件 SPI0 主机控制器驱动
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <soc/nuc472_soc.h>
#include <kernel/errno.h>
#include <kernel/device.h>
#include <kernel/mutex.h>
#include <drivers/spi.h>

#if defined(CONFIG_DEVICE_TREE)
#include <nos/devicetree.h>
#include <drivers/spi_dt.h>
#endif

#ifdef NUC472_HOST_TEST
void host_test_spi_write_tx(NUC472_SPI_Type *spi, uint8_t tx);
#endif

struct nuc472_spi_config {
    NUC472_SPI_Type *regs;
    uint32_t         irq;
};

struct nuc472_spi_data {
    struct mutex lock;
    uint32_t     pclk_hz;
};

static uint32_t nuc472_spi_calc_clkdiv(uint32_t pclk, uint32_t freq)
{
    if (freq == 0U || freq >= pclk) {
        return 0U;
    }
    /* NUC472 官方公式: f_spi = clk_src / (CLKDIV + 1)
     * 向上取整安全分频: div = ceil(pclk / freq) - 1 */
    uint32_t div = (pclk + freq - 1U) / freq - 1U;
    if (div > 255U) {
        div = 255U;
    }
    return div;
}

static uint32_t nuc472_spi_mode_ctl(uint8_t mode)
{
    uint32_t ctl = 0U;
    switch (mode & 0x03U) {
    case 0:
        ctl = NUC472_SPI_CTL_TXNEG;
        break;
    case 1:
        ctl = NUC472_SPI_CTL_RXNEG;
        break;
    case 2:
        ctl = NUC472_SPI_CTL_CLKPOL | NUC472_SPI_CTL_RXNEG;
        break;
    case 3:
        ctl = NUC472_SPI_CTL_CLKPOL | NUC472_SPI_CTL_TXNEG;
        break;
    default:
        break;
    }
    return ctl;
}

static int nuc472_spi_exchange_byte(NUC472_SPI_Type *spi, uint8_t tx, uint8_t *rx)
{
    uint32_t timeout = 1000000U;
    while ((spi->STATUS & NUC472_SPI_STATUS_TXFULL) != 0U) {
        if (--timeout == 0U) {
            return -ETIMEDOUT;
        }
    }

#ifdef NUC472_HOST_TEST
    host_test_spi_write_tx(spi, tx);
#else
    spi->TX = tx;
#endif

    timeout = 1000000U;
    while ((spi->STATUS & NUC472_SPI_STATUS_RXEMPTY) != 0U) {
        if (--timeout == 0U) {
            return -ETIMEDOUT;
        }
    }

    uint32_t val = spi->RX;
    if (rx != NULL) {
        *rx = (uint8_t)val;
    }
    return 0;
}

static int nuc472_spi_session_begin(const struct device *dev,
                                    const struct spi_config *config)
{
    if (dev == NULL || config == NULL) {
        return -EINVAL;
    }
    if (config->word_size != 8U || config->mode > 3U || config->frequency == 0U) {
        return -EINVAL;
    }
    if (config->chip_select != 0U) {
        return -EINVAL;
    }

    const struct nuc472_spi_config *cfg = dev->config;
    struct nuc472_spi_data *data = dev->data;
    NUC472_SPI_Type *spi = cfg->regs;

    mutex_lock(&data->lock);

    /* 1. 计算时钟分频并写入 CLKDIV */
    uint32_t clkdiv = nuc472_spi_calc_clkdiv(data->pclk_hz, config->frequency);
    spi->CLKDIV = clkdiv;

    /* 2. 配置 SPI_CTL: 启用、8 位传输与工作模式 */
    uint32_t ctl = NUC472_SPI_CTL_SPIEN |
                   (8U << NUC472_SPI_CTL_DWIDTH_Pos) |
                   nuc472_spi_mode_ctl(config->mode);
    spi->CTL = ctl;

    /* 3. 拉低 SS0 片选使能从机 */
    spi->SSCTL |= NUC472_SPI_SSCTL_SS0;

    return 0;
}

static int nuc472_spi_session_transfer(const struct device *dev,
                                       const struct spi_buf *buffers,
                                       size_t count)
{
    if (dev == NULL || buffers == NULL || count == 0U) {
        return -EINVAL;
    }

    const struct nuc472_spi_config *cfg = dev->config;
    NUC472_SPI_Type *spi = cfg->regs;

    for (size_t i = 0; i < count; i++) {
        const uint8_t *tx = (const uint8_t *)buffers[i].tx;
        uint8_t *rx = (uint8_t *)buffers[i].rx;
        size_t len = buffers[i].len;

        for (size_t j = 0; j < len; j++) {
            uint8_t tx_val = (tx != NULL) ? tx[j] : 0xFFU;
            uint8_t rx_val = 0U;
            int ret = nuc472_spi_exchange_byte(spi, tx_val, &rx_val);
            if (ret != 0) {
                return ret;
            }
            if (rx != NULL) {
                rx[j] = rx_val;
            }
        }
    }
    return 0;
}

static int nuc472_spi_session_end(const struct device *dev)
{
    if (dev == NULL) {
        return -EINVAL;
    }
    const struct nuc472_spi_config *cfg = dev->config;
    struct nuc472_spi_data *data = dev->data;
    NUC472_SPI_Type *spi = cfg->regs;

    /* 等待发送排空与总线空闲 */
    uint32_t timeout = 1000000U;
    while ((spi->STATUS & NUC472_SPI_STATUS_BUSY) != 0U) {
        if (--timeout == 0U) {
            break;
        }
    }

    /* 释放片选 SS0 */
    spi->SSCTL &= ~NUC472_SPI_SSCTL_SS0;

    mutex_unlock(&data->lock);
    return 0;
}

static int nuc472_spi_session_end_with_idle(const struct device *dev, size_t bytes)
{
    if (dev == NULL) {
        return -EINVAL;
    }
    const struct nuc472_spi_config *cfg = dev->config;
    struct nuc472_spi_data *data = dev->data;
    NUC472_SPI_Type *spi = cfg->regs;

    /* 等待发送排空与总线空闲，确保最后一个传输字节完成 */
    uint32_t timeout = 1000000U;
    while ((spi->STATUS & NUC472_SPI_STATUS_BUSY) != 0U) {
        if (--timeout == 0U) {
            break;
        }
    }

    /* 释放片选 SS0 */
    spi->SSCTL &= ~NUC472_SPI_SSCTL_SS0;

    /* 发送空闲时钟 */
    for (size_t i = 0; i < bytes; i++) {
        uint8_t ignored = 0U;
        (void)nuc472_spi_exchange_byte(spi, 0xFFU, &ignored);
    }

    mutex_unlock(&data->lock);
    return 0;
}

static int nuc472_spi_clock_idle(const struct device *dev,
                                 const struct spi_config *config,
                                 size_t bytes)
{
    if (dev == NULL || config == NULL || bytes == 0U) {
        return -EINVAL;
    }
    const struct nuc472_spi_config *cfg = dev->config;
    struct nuc472_spi_data *data = dev->data;
    NUC472_SPI_Type *spi = cfg->regs;

    mutex_lock(&data->lock);

    /* 保持片选为释放状态 */
    spi->SSCTL &= ~NUC472_SPI_SSCTL_SS0;

    uint32_t clkdiv = nuc472_spi_calc_clkdiv(data->pclk_hz, config->frequency);
    spi->CLKDIV = clkdiv;
    spi->CTL = NUC472_SPI_CTL_SPIEN |
               (8U << NUC472_SPI_CTL_DWIDTH_Pos) |
               nuc472_spi_mode_ctl(config->mode);

    for (size_t i = 0; i < bytes; i++) {
        uint8_t ignored = 0U;
        int ret = nuc472_spi_exchange_byte(spi, 0xFFU, &ignored);
        if (ret != 0) {
            mutex_unlock(&data->lock);
            return ret;
        }
    }

    mutex_unlock(&data->lock);
    return 0;
}

static int nuc472_spi_transceive(const struct device *dev,
                                 const struct spi_config *config,
                                 const struct spi_buf *buffers,
                                 size_t count)
{
    int ret = nuc472_spi_session_begin(dev, config);
    if (ret != 0) {
        return ret;
    }
    ret = nuc472_spi_session_transfer(dev, buffers, count);
    int end_ret = nuc472_spi_session_end(dev);
    return (ret != 0) ? ret : end_ret;
}

const struct spi_driver_api nuc472_spi_driver_api = {
    .transceive = nuc472_spi_transceive,
    .session_begin = nuc472_spi_session_begin,
    .session_transfer = nuc472_spi_session_transfer,
    .session_end = nuc472_spi_session_end,
    .session_end_with_idle = nuc472_spi_session_end_with_idle,
    .clock_idle = nuc472_spi_clock_idle,
};

static const struct nuc472_spi_config nuc472_spi0_cfg = {
    .regs = NUC472_SPI0,
    .irq  = 96U,
};

static struct nuc472_spi_data nuc472_spi0_data;

static int nuc472_spi_init(struct device *dev)
{
    if (dev == NULL || dev->config == NULL || dev->data == NULL) {
        return -EINVAL;
    }
    struct nuc472_spi_data *data = dev->data;
    mutex_init(&data->lock);
    data->pclk_hz = nuc472_clk_get_pclk_hz();

    /* 显式锁定 SPI0 外设时钟源为 PCLK (0=PLL, 1=PCLK) */
    nuc472_sys_unlock();
    NUC472_CLK->CLKSEL1 = (NUC472_CLK->CLKSEL1 & ~NUC472_CLK_CLKSEL1_SPI0SEL_Msk) |
                          NUC472_CLK_CLKSEL1_SPI0SEL_PCLK;

#ifndef NUC472_HOST_TEST
    /* 使能 SPI0 模块时钟与引脚配置 */
    NUC472_CLK->APBCLK0 |= NUC472_CLK_APBCLK0_SPI0CKEN;

    /* 复位 SPI0 外设控制器 */
    NUC472_SYS->IPRST1 |= NUC472_SYS_IPRST1_SPI0RST_Msk;
    NUC472_SYS->IPRST1 &= ~NUC472_SYS_IPRST1_SPI0RST_Msk;
#endif
    nuc472_sys_lock();

#if defined(CONFIG_DEVICE_TREE)
    const struct nos_dt_node *node = nos_dt_find_compatible("nuvoton,nuc472-spi");
    if (node != NULL) {
        dev->dt_node = node;
    }
#endif

    dev->ready = true;
    return 0;
}

DEVICE_DEFINE(nuc472_spi0, "SPI0", nuc472_spi_init,
              &nuc472_spi0_cfg, &nuc472_spi0_data,
              &nuc472_spi_driver_api, POST_KERNEL);
