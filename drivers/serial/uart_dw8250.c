/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * Synopsys DesignWare APB UART (DW-8250) 驱动（适配 RK3588 UART2 等工业总线）
 * 支持早期无锁轮询、Panic 同步冲刷及 ARM64 GIC + kfifo 中断异步接收。
 */

#include <drivers/serial.h>
#include <kernel/device.h>
#include <uapi/nos/errno.h>
#include <stdint.h>

#if defined(CONFIG_ARCH_ARM64) && !defined(__HOST_TEST__)
#include <kernel/kernel.h>
#include <kernel/irq.h>
#include <kernel/spinlock.h>
#include <lib/kfifo.h>
#include <nos/hal_arch.h>
#include <nos/notification.h>
#include <asm/mmu.h>
#include <asm/irq.h>
#endif

#define DW8250_RBR          0x00U
#define DW8250_THR          0x00U
#define DW8250_DLL          0x00U
#define DW8250_IER          0x04U
#define DW8250_DLH          0x04U
#define DW8250_IIR          0x08U
#define DW8250_FCR          0x08U
#define DW8250_LCR          0x0CU
#define DW8250_MCR          0x10U
#define DW8250_LSR          0x14U
#define DW8250_USR          0x7CU

#define DW8250_IER_ERBFI    (1U << 0)
#define DW8250_FCR_FIFO_EN  (1U << 0)
#define DW8250_FCR_RXSR     (1U << 1)
#define DW8250_FCR_TXSR     (1U << 2)
#define DW8250_LCR_WLS_8    0x03U
#define DW8250_LCR_DLAB     0x80U
#define DW8250_LSR_DR       (1U << 0)
#define DW8250_LSR_THRE     (1U << 5)

#ifndef CONFIG_RK3588_UART2_BASE
#define CONFIG_RK3588_UART2_BASE    0xFEB50000UL
#endif
#ifndef CONFIG_RK3588_UART_CLK_HZ
#define CONFIG_RK3588_UART_CLK_HZ   24000000U
#endif
#ifndef CONFIG_UART_BAUD_RATE
#define CONFIG_UART_BAUD_RATE       1500000U
#endif
#ifndef CONFIG_UART_POLL_TIMEOUT
#define CONFIG_UART_POLL_TIMEOUT    1000000U
#endif
#ifndef CONFIG_UART_RX_BUFFER_SIZE
#define CONFIG_UART_RX_BUFFER_SIZE  256U
#endif
#ifndef CONFIG_RK3588_UART2_IRQ
#define CONFIG_RK3588_UART2_IRQ     365U
#endif

struct uart_dw8250_config {
    uintptr_t base;
    uint32_t  clock_hz;
    uint32_t  baud_rate;
    uint32_t  poll_timeout;
    uint32_t  irq_num;
};

#if defined(CONFIG_ARCH_ARM64) && !defined(__HOST_TEST__)
struct uart_dw8250_data {
    volatile uint8_t       *vbase;
    struct kfifo            rx_fifo;
    unsigned char           rx_buf[CONFIG_UART_RX_BUFFER_SIZE];
    struct nos_notification rx_ready;
    spinlock_t              lock;
    bool                    irq_ready;
    uint32_t                rx_dropped;
};

static struct uart_dw8250_data g_dw8250_data;
#endif

static volatile uint32_t *dw8250_reg(const struct device *dev, uintptr_t offset)
{
    const struct uart_dw8250_config *cfg = dev->config;
#if defined(CONFIG_ARCH_ARM64) && !defined(__HOST_TEST__)
    struct uart_dw8250_data *data = dev->data;
    if (data != NULL && data->vbase != NULL) {
        return (volatile uint32_t *)(data->vbase + offset);
    }
#endif
    return (volatile uint32_t *)(cfg->base + offset);
}

static int uart_dw8250_poll_out(const struct device *dev, unsigned char ch)
{
    const struct uart_dw8250_config *cfg = dev->config;
    uint32_t remaining = cfg->poll_timeout;

    while ((*dw8250_reg(dev, DW8250_LSR) & DW8250_LSR_THRE) == 0U) {
        if (remaining-- == 0U) {
            return -ETIMEDOUT;
        }
    }
    *dw8250_reg(dev, DW8250_THR) = (uint32_t)ch;
    return 0;
}

static int uart_dw8250_poll_in(const struct device *dev, unsigned char *ch)
{
    if ((*dw8250_reg(dev, DW8250_LSR) & DW8250_LSR_DR) == 0U) {
        return -EAGAIN;
    }
    *ch = (unsigned char)(*dw8250_reg(dev, DW8250_RBR) & 0xFFU);
    return 0;
}

static ssize_t uart_dw8250_write(const struct device *dev,
                                 const unsigned char *buf,
                                 size_t len)
{
    size_t written;

    for (written = 0U; written < len; written++) {
        int rc = uart_dw8250_poll_out(dev, buf[written]);
        if (rc < 0) {
            return (written != 0U) ? (ssize_t)written : rc;
        }
    }
    return (ssize_t)written;
}

#if defined(CONFIG_ARCH_ARM64) && !defined(__HOST_TEST__)
static void uart_dw8250_irq_handler(uint32_t intid, void *ctx)
{
    const struct device *dev = (const struct device *)ctx;
    struct uart_dw8250_data *data;
    bool got_data = false;

    (void)intid;
    if (dev == NULL || dev->data == NULL) {
        return;
    }
    data = dev->data;

    spin_lock_irq(&data->lock);
    while ((*dw8250_reg(dev, DW8250_LSR) & DW8250_LSR_DR) != 0U) {
        unsigned char ch = (unsigned char)(*dw8250_reg(dev, DW8250_RBR) & 0xFFU);
        if (kfifo_in(&data->rx_fifo, &ch, 1U) == 0U) {
            data->rx_dropped++;
        } else {
            got_data = true;
        }
    }
    spin_unlock_irq(&data->lock);

    if (got_data && kernel_running) {
        (void)nos_notify_signal_obj(&data->rx_ready, 1ULL);
    }
}
#endif

static ssize_t uart_dw8250_read(const struct device *dev,
                                unsigned char *buf,
                                size_t len,
                                bool wait)
{
    const struct uart_dw8250_config *cfg = dev->config;
    uint32_t remaining = cfg->poll_timeout;
    size_t received = 0U;

#if defined(CONFIG_ARCH_ARM64) && !defined(__HOST_TEST__)
    struct uart_dw8250_data *data = dev->data;
    if (len == 0U) {
        return 0;
    }
    if (data != NULL && data->irq_ready) {
        for (;;) {
            uint64_t bits = 0ULL;
            unsigned int out_cnt;

            spin_lock_irq(&data->lock);
            out_cnt = kfifo_out(&data->rx_fifo, buf, (unsigned int)len);
            spin_unlock_irq(&data->lock);
            if (out_cnt > 0U) {
                return (ssize_t)out_cnt;
            }
            if (!wait || hal_in_irq()) {
                return -EAGAIN;
            }
            int rc = nos_notify_wait_obj(&data->rx_ready, &bits, NOS_WAIT_FOREVER);
            if (rc < 0) {
                return rc;
            }
        }
    }
#endif

    while (received < len) {
        int rc = uart_dw8250_poll_in(dev, &buf[received]);
        if (rc == 0) {
            received++;
        } else if (received != 0U) {
            break;
        } else if (!wait) {
            return rc;
        } else if (remaining-- == 0U) {
            return -ETIMEDOUT;
        }
    }
    return (ssize_t)received;
}

static void uart_dw8250_panic_begin(const struct device *dev)
{
    *dw8250_reg(dev, DW8250_IER) = 0U;
}

static void uart_dw8250_panic_write(const struct device *dev,
                                    const unsigned char *buf,
                                    size_t len)
{
    for (size_t i = 0U; i < len; i++) {
        if (uart_dw8250_poll_out(dev, buf[i]) < 0) {
            break;
        }
    }
}

static int uart_dw8250_set_baud(const struct device *dev, uint32_t clock_hz, uint32_t baud)
{
    uint32_t divisor;

    if (clock_hz == 0U || baud == 0U) {
        return -EINVAL;
    }
    divisor = (clock_hz + (8U * baud)) / (16U * baud);
    if (divisor == 0U) {
        divisor = 1U;
    }

    *dw8250_reg(dev, DW8250_LCR) = DW8250_LCR_DLAB | DW8250_LCR_WLS_8;
    *dw8250_reg(dev, DW8250_DLL) = divisor & 0xFFU;
    *dw8250_reg(dev, DW8250_DLH) = (divisor >> 8) & 0xFFU;
    *dw8250_reg(dev, DW8250_LCR) = DW8250_LCR_WLS_8;
    return 0;
}

static int uart_dw8250_configure(const struct device *dev, const struct uart_config *cfg)
{
    const struct uart_dw8250_config *hw = dev->config;

    if (cfg == NULL || cfg->baud_rate == 0U) {
        return -EINVAL;
    }
    return uart_dw8250_set_baud(dev, hw->clock_hz, cfg->baud_rate);
}

static int uart_dw8250_get_config(const struct device *dev, struct uart_config *cfg)
{
    const struct uart_dw8250_config *hw = dev->config;

    if (cfg == NULL) {
        return -EINVAL;
    }
    cfg->baud_rate = hw->baud_rate;
    cfg->data_bits = UART_DATA_BITS_8;
    cfg->parity = UART_PARITY_NONE;
    cfg->stop_bits = UART_STOP_BITS_1;
    cfg->flow_ctrl = UART_FLOW_NONE;
    return 0;
}

static int uart_dw8250_init(struct device *dev)
{
    const struct uart_dw8250_config *hw = dev->config;
    int rc;

    if (hw == NULL || hw->clock_hz == 0U || hw->baud_rate == 0U) {
        return -EINVAL;
    }

#if defined(CONFIG_ARCH_ARM64) && !defined(__HOST_TEST__)
    if (dev->data != NULL) {
        struct uart_dw8250_data *data = dev->data;
        data->vbase = (volatile uint8_t *)arm64_ioremap(hw->base, 0x1000UL);
    }
#endif

    *dw8250_reg(dev, DW8250_IER) = 0U;
    *dw8250_reg(dev, DW8250_FCR) = DW8250_FCR_FIFO_EN | DW8250_FCR_RXSR | DW8250_FCR_TXSR;
    rc = uart_dw8250_set_baud(dev, hw->clock_hz, hw->baud_rate);
    if (rc != 0) {
        return rc;
    }
    *dw8250_reg(dev, DW8250_MCR) = 0x03U;

#if defined(CONFIG_ARCH_ARM64) && !defined(__HOST_TEST__)
    if (dev->data != NULL && hw->irq_num > 0U) {
        struct uart_dw8250_data *data = dev->data;
        spin_lock_init(&data->lock);
        (void)kfifo_init(&data->rx_fifo, data->rx_buf, sizeof(data->rx_buf), 1U);
        (void)nos_notification_init(&data->rx_ready);
        if (nos_irq_register(hw->irq_num, uart_dw8250_irq_handler, dev) == 0) {
            hal_irq_enable(hw->irq_num);
            *dw8250_reg(dev, DW8250_IER) = DW8250_IER_ERBFI;
            data->irq_ready = true;
        }
    }
#endif
    return 0;
}

static const struct uart_driver_api uart_dw8250_api = {
    .poll_out    = uart_dw8250_poll_out,
    .poll_in     = uart_dw8250_poll_in,
    .write       = uart_dw8250_write,
    .read        = uart_dw8250_read,
    .configure   = uart_dw8250_configure,
    .get_config  = uart_dw8250_get_config,
    .panic_begin = uart_dw8250_panic_begin,
    .panic_write = uart_dw8250_panic_write,
};

#ifndef __HOST_TEST__
static const struct uart_dw8250_config rk3588_uart2_config = {
    .base         = CONFIG_RK3588_UART2_BASE,
    .clock_hz     = CONFIG_RK3588_UART_CLK_HZ,
    .baud_rate    = CONFIG_UART_BAUD_RATE,
    .poll_timeout = CONFIG_UART_POLL_TIMEOUT,
    .irq_num      = CONFIG_RK3588_UART2_IRQ,
};

DEVICE_DEFINE(rk3588_uart2, "UART1", uart_dw8250_init, &rk3588_uart2_config,
              &g_dw8250_data, &uart_dw8250_api, PRE_KERNEL_1);
#endif
