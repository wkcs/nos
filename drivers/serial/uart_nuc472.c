/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * Author: Nick Hu <huqihan@live.com>
 * NUC472 UART0 polling console on PG.1 (RX) and PG.2 (TX).
 */
#include <drivers/serial.h>
#include <kernel/device.h>
#include <stdint.h>

#define REG32(addr) (*(volatile uint32_t *)(addr))
#define SYS_REGLCTL REG32(0x40000100UL)
#define SYS_GPG_MFPL REG32(0x40000060UL)
#define CLK_APBCLK0 REG32(0x40000208UL)
#define CLK_CLKSEL1 REG32(0x40000214UL)
#define CLK_CLKDIV0 REG32(0x40000220UL)
#define UART_DAT REG32(0x40070000UL)
#define UART_LINE REG32(0x4007000cUL)
#define UART_FIFOSTS REG32(0x40070018UL)
#define UART_BAUD REG32(0x40070024UL)
#define UART_FUNCSEL REG32(0x40070030UL)
#define UART_RXEMPTY (1U << 14)
#define UART_TXFULL (1U << 23)
#define UART_TIMEOUT 1000000U

static int nuc472_poll_out(const struct device *dev, unsigned char ch)
{
    uint32_t remaining = UART_TIMEOUT;
    (void)dev;
    while ((UART_FIFOSTS & UART_TXFULL) != 0) {
        if (remaining-- == 0) {
            return -ETIMEDOUT;
        }
    }
    UART_DAT = ch;
    return 0;
}

static int nuc472_poll_in(const struct device *dev, unsigned char *ch)
{
    (void)dev;
    if ((UART_FIFOSTS & UART_RXEMPTY) != 0) {
        return -EAGAIN;
    }
    *ch = (unsigned char)UART_DAT;
    return 0;
}

static int nuc472_write(const struct device *dev, const unsigned char *buf, unsigned int len)
{
    unsigned int i;
    for (i = 0; i < len; i++) {
        int rc = nuc472_poll_out(dev, buf[i]);
        if (rc < 0) {
            return i != 0 ? (int)i : rc;
        }
    }
    return (int)i;
}

static int nuc472_read(const struct device *dev, unsigned char *buf, unsigned int len, bool wait)
{
    uint32_t remaining = UART_TIMEOUT;
    unsigned int i = 0;
    while (i < len) {
        int rc = nuc472_poll_in(dev, &buf[i]);
        if (rc == 0) {
            i++;
        } else if (i != 0) {
            break;
        } else if (!wait) {
            return rc;
        } else if (remaining-- == 0) {
            return -ETIMEDOUT;
        }
    }
    return (int)i;
}

static int nuc472_init(struct device *dev)
{
    (void)dev;
    SYS_REGLCTL = 0x59;
    SYS_REGLCTL = 0x16;
    SYS_REGLCTL = 0x88;
    CLK_APBCLK0 |= 1U << 16;
    CLK_CLKSEL1 = (CLK_CLKSEL1 & ~(3U << 24)) | (3U << 24);
    CLK_CLKDIV0 &= ~(15U << 8);
    SYS_GPG_MFPL = (SYS_GPG_MFPL & ~(0xffU << 4)) | (0x11U << 4);
    SYS_REGLCTL = 0;
    UART_FUNCSEL = 0;
    UART_LINE = 3;
    UART_BAUD = 0x30000000U | ((22118400U + 57600U) / 115200U - 2U);
    return 0;
}

static int nuc472_panic_begin(const struct device *dev)
{
    (void)dev;
    return 0;
}

static const struct uart_driver_api nuc472_uart_api = {
    .poll_out = nuc472_poll_out,
    .poll_in = nuc472_poll_in,
    .write = nuc472_write,
    .write_record = nuc472_write,
    .read = nuc472_read,
    .panic_begin = nuc472_panic_begin,
    .panic_write = nuc472_write,
};

DEVICE_DEFINE(nuc472_uart0, "UART1", nuc472_init, NULL, NULL,
              &nuc472_uart_api, PRE_KERNEL_1);
