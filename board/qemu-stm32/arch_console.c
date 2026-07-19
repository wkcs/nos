/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/console.h>
#include <drivers/serial.h>
#include <kernel/device.h>
#include <kernel/errno.h>

static struct device *console_uart;

int consol_init(void)
{
    console_uart = device_get_binding("UART1");

    return device_is_ready(console_uart) ? 0 : -ENODEV;
}

int console_send_data(const char *buf, int len)
{
    int i;
    int ret;

    if (buf == NULL || len < 0) {
        return -EINVAL;
    }

    for (i = 0; i < len; i++) {
        ret = uart_poll_out(console_uart, (unsigned char)buf[i]);
        if (ret < 0) {
            return ret;
        }
    }

    return len;
}

int arch_console_putc(char c)
{
    return uart_poll_out(console_uart, (unsigned char)c);
}

static char arch_console_getc(void)
{
    unsigned char c;

    while (uart_poll_in(console_uart, &c) == -EAGAIN) {
    }

    return (char)c;
}

static void arch_console_send_log(void)
{
}

static struct console_ops zj_console_ops = {
    .init = consol_init,
    .write = console_send_data,
    .getc = arch_console_getc,
    .putc = arch_console_putc,
    .send_log = arch_console_send_log,
};
console_register(tty0, &zj_console_ops);
