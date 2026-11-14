/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NUC472 硬件 I2C3 主机控制器驱动
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <soc/nuc472_soc.h>
#include <kernel/errno.h>
#include <kernel/device.h>
#include <kernel/mutex.h>
#include <drivers/i2c.h>

#if defined(CONFIG_DEVICE_TREE)
#include <nos/devicetree.h>
#endif

#ifdef NUC472_HOST_TEST
void host_test_i2c_step(NUC472_I2C_Type *i2c);
#endif

struct nuc472_i2c_config {
    NUC472_I2C_Type *regs;
    uint32_t         irq;
};

struct nuc472_i2c_data {
    struct mutex lock;
};

static int nuc472_i2c_wait_si(NUC472_I2C_Type *i2c)
{
    uint32_t timeout = 100000U;

#ifdef NUC472_HOST_TEST
    host_test_i2c_step(i2c);
#endif

    while ((i2c->CTL & NUC472_I2C_CTL_SI) == 0U) {
        if (--timeout == 0U) {
            /* 超时安全退出：发送 STOP 并清除 SI，防止总线锁死 */
            i2c->CTL = NUC472_I2C_CTL_I2CEN | NUC472_I2C_CTL_STO | NUC472_I2C_CTL_SI;
            return -ETIMEDOUT;
        }
    }
    return 0;
}

static void nuc472_i2c_send_stop(NUC472_I2C_Type *i2c)
{
    i2c->CTL = NUC472_I2C_CTL_I2CEN | NUC472_I2C_CTL_STO | NUC472_I2C_CTL_SI;
#ifdef NUC472_HOST_TEST
    host_test_i2c_step(i2c);
#endif
}

static int nuc472_i2c_transfer_msg(NUC472_I2C_Type *i2c, uint16_t address,
                                   const struct i2c_msg *msg, bool is_first)
{
    bool is_read = (msg->flags & I2C_MSG_READ) != 0U;
    bool is_restart = (msg->flags & I2C_MSG_RESTART) != 0U;
    int ret;

    /* 1. 发送 START 或 Repeated START 信号 */
    if (is_first || is_restart) {
        i2c->CTL = NUC472_I2C_CTL_I2CEN | NUC472_I2C_CTL_STA | NUC472_I2C_CTL_SI;
        ret = nuc472_i2c_wait_si(i2c);
        if (ret != 0) {
            return ret;
        }

        uint32_t st = i2c->STATUS;
        if (st != 0x08U && st != 0x10U) {
            nuc472_i2c_send_stop(i2c);
            return -EIO;
        }

        /* 2. 发送 SLA+W 或 SLA+R */
        i2c->DAT = (uint32_t)(((uint32_t)address << 1U) | (is_read ? 1U : 0U));
        i2c->CTL = NUC472_I2C_CTL_I2CEN | NUC472_I2C_CTL_SI;
        ret = nuc472_i2c_wait_si(i2c);
        if (ret != 0) {
            return ret;
        }

        st = i2c->STATUS;
        if (!is_read) {
            if (st == 0x20U) {
                /* SLA+W 收到 NACK */
                nuc472_i2c_send_stop(i2c);
                return -ENXIO;
            }
            if (st != 0x18U) {
                nuc472_i2c_send_stop(i2c);
                return -EIO;
            }
        } else {
            if (st == 0x48U) {
                /* SLA+R 收到 NACK */
                nuc472_i2c_send_stop(i2c);
                return -ENXIO;
            }
            if (st != 0x40U) {
                nuc472_i2c_send_stop(i2c);
                return -EIO;
            }
        }
    }

    /* 3. 数据读写 */
    if (!is_read) {
        /* 写数据 */
        for (size_t i = 0; i < msg->len; i++) {
            i2c->DAT = msg->buf[i];
            i2c->CTL = NUC472_I2C_CTL_I2CEN | NUC472_I2C_CTL_SI;
            ret = nuc472_i2c_wait_si(i2c);
            if (ret != 0) {
                return ret;
            }
            if (i2c->STATUS != 0x28U) {
                nuc472_i2c_send_stop(i2c);
                return -EIO;
            }
        }
    } else {
        /* 读数据 */
        for (size_t i = 0; i < msg->len; i++) {
            bool last = (i == msg->len - 1U);
            if (last) {
                /* 最后一字节回复 NACK */
                i2c->CTL = NUC472_I2C_CTL_I2CEN | NUC472_I2C_CTL_SI;
            } else {
                /* 前续字节回复 ACK */
                i2c->CTL = NUC472_I2C_CTL_I2CEN | NUC472_I2C_CTL_SI | NUC472_I2C_CTL_AA;
            }
            ret = nuc472_i2c_wait_si(i2c);
            if (ret != 0) {
                return ret;
            }

            uint32_t st = i2c->STATUS;
            if (st != 0x50U && st != 0x58U) {
                nuc472_i2c_send_stop(i2c);
                return -EIO;
            }
            msg->buf[i] = (uint8_t)i2c->DAT;
        }
    }

    /* 4. 如果包含 STOP 标志，发送停止位 */
    if (msg->flags & I2C_MSG_STOP) {
        nuc472_i2c_send_stop(i2c);
    }
    return 0;
}

static int nuc472_i2c_transfer(const struct device *dev, uint16_t address,
                               const struct i2c_msg *messages, size_t count)
{
    if (dev == NULL || messages == NULL || count == 0U) {
        return -EINVAL;
    }

    const struct nuc472_i2c_config *cfg = dev->config;
    struct nuc472_i2c_data *data = dev->data;
    NUC472_I2C_Type *i2c = cfg->regs;

    mutex_lock(&data->lock);

    int ret = 0;
    for (size_t i = 0; i < count; i++) {
        ret = nuc472_i2c_transfer_msg(i2c, address, &messages[i], (i == 0));
        if (ret != 0) {
            break;
        }
    }

    mutex_unlock(&data->lock);
    return ret;
}

const struct i2c_driver_api nuc472_i2c_driver_api = {
    .transfer = nuc472_i2c_transfer,
};

static const struct nuc472_i2c_config nuc472_i2c3_cfg = {
    .regs = NUC472_I2C3,
    .irq  = 115U,
};

static struct nuc472_i2c_data nuc472_i2c3_data;

static uint32_t nuc472_i2c_calc_clkdiv(uint32_t pclk, uint32_t bus_clk)
{
    if (bus_clk == 0U) {
        return 0U;
    }
    /* NUC472 官方公式: f_i2c = PCLK / (4 * (CLKDIV + 1))
     * 因此: CLKDIV = PCLK / (4 * bus_clk) - 1 */
    uint32_t div = pclk / (4U * bus_clk);
    if (div > 0U) {
        div -= 1U;
    }
    if (div > 255U) {
        div = 255U;
    }
    return div;
}

static int nuc472_i2c_init(struct device *dev)
{
    if (dev == NULL || dev->config == NULL || dev->data == NULL) {
        return -EINVAL;
    }
    const struct nuc472_i2c_config *cfg = dev->config;
    struct nuc472_i2c_data *data = dev->data;
    NUC472_I2C_Type *i2c = cfg->regs;
    mutex_init(&data->lock);

#ifndef NUC472_HOST_TEST
    /* 使能 I2C3 时钟与复位控制器 */
    NUC472_CLK->APBCLK0 |= NUC472_CLK_APBCLK0_I2C3CKEN;
    NUC472_SYS->IPRST1 |= NUC472_SYS_IPRST1_I2C3RST_Msk;
    NUC472_SYS->IPRST1 &= ~NUC472_SYS_IPRST1_I2C3RST_Msk;
#endif

    /* 计算 100 kHz 标准模式时钟分频并写入 CLKDIV */
    uint32_t pclk = nuc472_clk_get_pclk_hz();
    i2c->CLKDIV = nuc472_i2c_calc_clkdiv(pclk, 100000U);

    /* 使能 I2C 控制器 */
    i2c->CTL |= NUC472_I2C_CTL_I2CEN;

#if defined(CONFIG_DEVICE_TREE)
    const struct nos_dt_node *node = nos_dt_find_compatible("nuvoton,nuc472-i2c");
    if (node != NULL) {
        dev->dt_node = node;
    }
#endif

    dev->ready = true;
    return 0;
}

DEVICE_DEFINE(nuc472_i2c3, "I2C3", nuc472_i2c_init,
              &nuc472_i2c3_cfg, &nuc472_i2c3_data,
              &nuc472_i2c_driver_api, POST_KERNEL);
