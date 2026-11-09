/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * PL011 polling transport for QEMU virt early boot and panic output.
 * The ARM64 interrupt and scheduling paths are not yet available.
 */

#include <drivers/serial.h>
#include <kernel/device.h>
#include <stdint.h>

#define PL011_DR        0x000U
#define PL011_FR        0x018U
#define PL011_IBRD      0x024U
#define PL011_FBRD      0x028U
#define PL011_LCRH      0x02cU
#define PL011_CR        0x030U
#define PL011_IMSC      0x038U
#define PL011_ICR       0x044U

#define PL011_FR_RXFE   (1U << 4)
#define PL011_FR_TXFF   (1U << 5)
#define PL011_LCRH_FEN  (1U << 4)
#define PL011_LCRH_8BIT (3U << 5)
#define PL011_CR_UARTEN (1U << 0)
#define PL011_CR_TXE    (1U << 8)
#define PL011_CR_RXE    (1U << 9)

#ifndef CONFIG_UART_BAUD_RATE
#define CONFIG_UART_BAUD_RATE 115200
#endif
#ifndef CONFIG_PL011_CLOCK_HZ
#define CONFIG_PL011_CLOCK_HZ 24000000
#endif
#ifndef CONFIG_UART_POLL_TIMEOUT
#define CONFIG_UART_POLL_TIMEOUT 1000000
#endif

struct uart_pl011_config {
    uintptr_t base;
    uint32_t clock_hz;
    uint32_t baud_rate;
    uint32_t poll_timeout;
};

static volatile uint32_t *pl011_reg(const struct uart_pl011_config *config,
                                    uintptr_t offset)
{
    return (volatile uint32_t *)(config->base + offset);
}

static int uart_pl011_poll_out(const struct device *dev, unsigned char ch)
{
    const struct uart_pl011_config *config = dev->config;
    uint32_t remaining = config->poll_timeout;

    while ((*pl011_reg(config, PL011_FR) & PL011_FR_TXFF) != 0) {
        if (remaining-- == 0) {
            return -ETIMEDOUT;
        }
    }
    *pl011_reg(config, PL011_DR) = ch;
    return 0;
}

static int uart_pl011_poll_in(const struct device *dev, unsigned char *ch)
{
    const struct uart_pl011_config *config = dev->config;

    if ((*pl011_reg(config, PL011_FR) & PL011_FR_RXFE) != 0) {
        return -EAGAIN;
    }
    *ch = (unsigned char)*pl011_reg(config, PL011_DR);
    return 0;
}

static ssize_t uart_pl011_write(const struct device *dev, const unsigned char *buf,
                                size_t len)
{
    size_t written;

    for (written = 0; written < len; written++) {
        int rc = uart_pl011_poll_out(dev, buf[written]);
        if (rc < 0) {
            return written != 0 ? (ssize_t)written : rc;
        }
    }
    return (ssize_t)written;
}

static ssize_t uart_pl011_read(const struct device *dev, unsigned char *buf,
                               size_t len, bool wait)
{
    const struct uart_pl011_config *config = dev->config;
    uint32_t remaining = config->poll_timeout;
    size_t received = 0;

    while (received < len) {
        int rc = uart_pl011_poll_in(dev, &buf[received]);
        if (rc == 0) {
            received++;
        } else if (received != 0) {
            break;
        } else if (!wait) {
            return rc;
        } else if (remaining-- == 0) {
            return -ETIMEDOUT;
        }
    }
    return (ssize_t)received;
}

static void uart_pl011_panic_begin(const struct device *dev)
{
    const struct uart_pl011_config *config = dev->config;
    *pl011_reg(config, PL011_IMSC) = 0;
}

static void uart_pl011_panic_write(const struct device *dev, const unsigned char *buf,
                                   size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (uart_pl011_poll_out(dev, buf[i]) < 0) {
            break;
        }
    }
}

static int uart_pl011_configure(const struct device *dev, const struct uart_config *cfg)
{
    (void)dev;
    (void)cfg;
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

    if (config->baud_rate == 0 || config->clock_hz == 0) {
        return -EINVAL;
    }
    divisor = ((uint64_t)config->clock_hz * 4U + config->baud_rate / 2U) /
              config->baud_rate;
    if (divisor < 64U || divisor > 4194303U) {
        return -EINVAL;
    }
    *pl011_reg(config, PL011_CR) = 0;
    *pl011_reg(config, PL011_IMSC) = 0;
    *pl011_reg(config, PL011_ICR) = 0x7ffU;
    *pl011_reg(config, PL011_IBRD) = (uint32_t)(divisor >> 6);
    *pl011_reg(config, PL011_FBRD) = (uint32_t)(divisor & 63U);
    *pl011_reg(config, PL011_LCRH) = PL011_LCRH_8BIT | PL011_LCRH_FEN;
    *pl011_reg(config, PL011_CR) = PL011_CR_UARTEN | PL011_CR_TXE | PL011_CR_RXE;
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

DEVICE_DEFINE(qemu_virt_pl011, "UART1", uart_pl011_init, &qemu_virt_pl011_config,
              NULL, &uart_pl011_api, PRE_KERNEL_1);
