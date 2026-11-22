/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM PrimeCell PL011 UART 驱动
 * 支持早期/Panic 无锁轮询与 ARM64 运行时中断驱动（GIC + kfifo + Notification 阻塞唤醒）。
 */

#include <drivers/serial.h>
#include <kernel/device.h>
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

#define PL011_DR        0x000U
#define PL011_FR        0x018U
#define PL011_IBRD      0x024U
#define PL011_FBRD      0x028U
#define PL011_LCRH      0x02CU
#define PL011_CR        0x030U
#define PL011_IMSC      0x038U
#define PL011_RIS       0x03CU
#define PL011_MIS       0x040U
#define PL011_ICR       0x044U

#define PL011_FR_RXFE   (1U << 4)
#define PL011_FR_TXFF   (1U << 5)
#define PL011_LCRH_FEN  (1U << 4)
#define PL011_LCRH_8BIT (3U << 5)
#define PL011_CR_UARTEN (1U << 0)
#define PL011_CR_TXE    (1U << 8)
#define PL011_CR_RXE    (1U << 9)
#define PL011_INT_RX    (1U << 4)
#define PL011_INT_RT    (1U << 6)

#ifndef CONFIG_UART_BAUD_RATE
#define CONFIG_UART_BAUD_RATE 115200
#endif
#ifndef CONFIG_PL011_CLOCK_HZ
#define CONFIG_PL011_CLOCK_HZ 24000000
#endif
#ifndef CONFIG_UART_POLL_TIMEOUT
#define CONFIG_UART_POLL_TIMEOUT 1000000
#endif
#ifndef CONFIG_UART_RX_BUFFER_SIZE
#define CONFIG_UART_RX_BUFFER_SIZE 256
#endif
#ifndef CONFIG_PL011_IRQ
#define CONFIG_PL011_IRQ 33U
#endif

struct uart_pl011_config {
    uintptr_t base;
    uint32_t  clock_hz;
    uint32_t  baud_rate;
    uint32_t  poll_timeout;
};

#if defined(CONFIG_ARCH_ARM64) && !defined(__HOST_TEST__)
struct uart_pl011_data {
    volatile uint8_t       *vbase;
    struct kfifo            rx_fifo;
    unsigned char           rx_buf[CONFIG_UART_RX_BUFFER_SIZE];
    struct nos_notification rx_ready;
    spinlock_t              lock;
    bool                    irq_ready;
    uint32_t                rx_dropped;
};

static struct uart_pl011_data g_qemu_virt_pl011_data;
#endif

static volatile uint32_t *pl011_reg(const struct device *dev, uintptr_t offset)
{
    const struct uart_pl011_config *config = dev->config;
#if defined(CONFIG_ARCH_ARM64) && !defined(__HOST_TEST__)
    struct uart_pl011_data *data = dev->data;
    if (data != NULL && data->vbase != NULL) {
        return (volatile uint32_t *)(data->vbase + offset);
    }
#endif
    return (volatile uint32_t *)(config->base + offset);
}

static int uart_pl011_poll_out(const struct device *dev, unsigned char ch)
{
    const struct uart_pl011_config *config = dev->config;
    uint32_t remaining = config->poll_timeout;

    while ((*pl011_reg(dev, PL011_FR) & PL011_FR_TXFF) != 0U) {
        if (remaining-- == 0U) {
            return -ETIMEDOUT;
        }
    }
    *pl011_reg(dev, PL011_DR) = ch;
    return 0;
}

static int uart_pl011_poll_in(const struct device *dev, unsigned char *ch)
{
    if ((*pl011_reg(dev, PL011_FR) & PL011_FR_RXFE) != 0U) {
        return -EAGAIN;
    }
    *ch = (unsigned char)*pl011_reg(dev, PL011_DR);
    return 0;
}

static ssize_t uart_pl011_write(const struct device *dev, const unsigned char *buf,
                                size_t len)
{
    size_t written;

    for (written = 0U; written < len; written++) {
        int rc = uart_pl011_poll_out(dev, buf[written]);
        if (rc < 0) {
            return written != 0U ? (ssize_t)written : rc;
        }
    }
    return (ssize_t)written;
}

#if defined(CONFIG_ARCH_ARM64) && !defined(__HOST_TEST__)
static void uart_pl011_irq_handler(uint32_t intid, void *ctx)
{
    const struct device *dev = (const struct device *)ctx;
    struct uart_pl011_data *data;
    bool got_data = false;

    (void)intid;
    if (dev == NULL || dev->data == NULL) {
        return;
    }
    data = dev->data;

    while ((*pl011_reg(dev, PL011_FR) & PL011_FR_RXFE) == 0U) {
        unsigned char ch = (unsigned char)*pl011_reg(dev, PL011_DR);
        spin_lock_irq(&data->lock);
        unsigned int accepted = kfifo_in(&data->rx_fifo, &ch, 1U);
        if (accepted == 0U) {
            data->rx_dropped++;
        } else {
            got_data = true;
        }
        spin_unlock_irq(&data->lock);
    }

    *pl011_reg(dev, PL011_ICR) = PL011_INT_RX | PL011_INT_RT;
    if (got_data && kernel_running) {
        (void)nos_notify_signal_obj(&data->rx_ready, 1ULL);
    }
}
#endif

static ssize_t uart_pl011_read(const struct device *dev, unsigned char *buf,
                               size_t len, bool wait)
{
    const struct uart_pl011_config *config = dev->config;
    uint32_t remaining = config->poll_timeout;
    size_t received = 0U;

#if defined(CONFIG_ARCH_ARM64) && !defined(__HOST_TEST__)
    struct uart_pl011_data *data = dev->data;
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
            if (!kernel_running) {
                int rc = uart_pl011_poll_in(dev, buf);
                if (rc == 0) {
                    return 1;
                }
                if (rc != -EAGAIN) {
                    return rc;
                }
                continue;
            }
            int rc = nos_notify_wait_obj(&data->rx_ready, &bits,
                                         NOS_IPC_TIMEOUT_INFINITE);
            if (rc < 0) {
                return rc;
            }
        }
    }
#endif

    while (received < len) {
        int rc = uart_pl011_poll_in(dev, &buf[received]);
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

static void uart_pl011_panic_begin(const struct device *dev)
{
    *pl011_reg(dev, PL011_IMSC) = 0U;
}

static void uart_pl011_panic_write(const struct device *dev, const unsigned char *buf,
                                   size_t len)
{
    for (size_t i = 0U; i < len; i++) {
        if (uart_pl011_poll_out(dev, buf[i]) < 0) {
            break;
        }
    }
}

static int uart_pl011_configure(const struct device *dev, const struct uart_config *cfg)
{
    (void)dev;
    if (cfg == NULL) {
        return -EINVAL;
    }
    return 0;
}

static int uart_pl011_get_config(const struct device *dev, struct uart_config *cfg)
{
    const struct uart_pl011_config *config = dev->config;

    if (cfg == NULL) {
        return -EINVAL;
    }
    cfg->baud_rate = config->baud_rate;
    cfg->data_bits = UART_DATA_BITS_8;
    cfg->parity = UART_PARITY_NONE;
    cfg->stop_bits = UART_STOP_BITS_1;
    cfg->flow_ctrl = UART_FLOW_NONE;
    return 0;
}

static int uart_pl011_init(struct device *dev)
{
    const struct uart_pl011_config *config = dev->config;
    uint64_t divisor;

    if (config->baud_rate == 0U || config->clock_hz == 0U) {
        return -EINVAL;
    }

#if defined(CONFIG_ARCH_ARM64) && !defined(__HOST_TEST__)
    if (dev->data != NULL) {
        struct uart_pl011_data *data = dev->data;
        data->vbase = (volatile uint8_t *)arm64_ioremap(config->base, 0x1000UL);
    }
#endif

    divisor = ((uint64_t)config->clock_hz * 4U + config->baud_rate / 2U) /
              config->baud_rate;
    if (divisor < 64U || divisor > 4194303U) {
        return -EINVAL;
    }
    *pl011_reg(dev, PL011_CR) = 0U;
    *pl011_reg(dev, PL011_IMSC) = 0U;
    *pl011_reg(dev, PL011_ICR) = 0x7FFU;
    *pl011_reg(dev, PL011_IBRD) = (uint32_t)(divisor >> 6);
    *pl011_reg(dev, PL011_FBRD) = (uint32_t)(divisor & 63U);
    *pl011_reg(dev, PL011_LCRH) = PL011_LCRH_8BIT | PL011_LCRH_FEN;
    *pl011_reg(dev, PL011_CR) = PL011_CR_UARTEN | PL011_CR_TXE | PL011_CR_RXE;

#if defined(CONFIG_ARCH_ARM64) && !defined(__HOST_TEST__)
    if (dev->data != NULL) {
        struct uart_pl011_data *data = dev->data;
        spin_lock_init(&data->lock);
        (void)kfifo_init(&data->rx_fifo, data->rx_buf, sizeof(data->rx_buf), 1U);
        (void)nos_notification_init(&data->rx_ready);
        if (nos_irq_register(CONFIG_PL011_IRQ, uart_pl011_irq_handler, dev) == 0) {
            hal_irq_enable(CONFIG_PL011_IRQ);
            /* 同时开启 FIFO 水位中断与接收超时中断，保障单字符键入及时唤醒 nShell */
            *pl011_reg(dev, PL011_IMSC) = PL011_INT_RX | PL011_INT_RT;
            data->irq_ready = true;
        }
    }
#endif
    return 0;
}

static const struct uart_driver_api uart_pl011_api = {
    .poll_out = uart_pl011_poll_out,
    .poll_in = uart_pl011_poll_in,
    .write = uart_pl011_write,
    .read = uart_pl011_read,
    .configure = uart_pl011_configure,
    .get_config = uart_pl011_get_config,
    .panic_begin = uart_pl011_panic_begin,
    .panic_write = uart_pl011_panic_write,
};

static const struct uart_pl011_config qemu_virt_pl011_config = {
    .base = 0x09000000UL,
    .clock_hz = CONFIG_PL011_CLOCK_HZ,
    .baud_rate = CONFIG_UART_BAUD_RATE,
    .poll_timeout = CONFIG_UART_POLL_TIMEOUT,
};

#if defined(CONFIG_ARCH_ARM64) && !defined(__HOST_TEST__)
DEVICE_DEFINE(qemu_virt_pl011, "UART1", uart_pl011_init, &qemu_virt_pl011_config,
              &g_qemu_virt_pl011_data, &uart_pl011_api, PRE_KERNEL_1);
#else
DEVICE_DEFINE(qemu_virt_pl011, "UART1", uart_pl011_init, &qemu_virt_pl011_config,
              NULL, &uart_pl011_api, PRE_KERNEL_1);
#endif
