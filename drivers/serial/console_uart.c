/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * Console adapter for the configured UART device.
 */

#include <drivers/serial.h>
#include <kernel/console.h>
#include <kernel/device.h>
#include <kernel/errno.h>
#ifdef CONFIG_TTY
#include <nos/tty.h>
#endif

static struct device *console_uart;
#ifdef CONFIG_TTY
static struct nos_tty console_tty;

static int tty_uart_read(void *context, unsigned char *buf, unsigned int len, bool wait)
{
    return uart_read(context, buf, len, wait);
}

static int tty_uart_write(void *context, const unsigned char *buf, unsigned int len)
{
    return uart_write(context, buf, len);
}

static const struct nos_tty_ops tty_uart_ops = {
    .read = tty_uart_read,
    .write = tty_uart_write,
};

struct nos_tty *nos_console_tty(void)
{
    return device_is_ready(console_uart) ? &console_tty : NULL;
}
#endif

static int uart_console_init(void)
{
    console_uart = device_get_binding("UART1");
    if (!device_is_ready(console_uart)) {
        return -ENODEV;
    }
#ifdef CONFIG_TTY
    return nos_tty_init(&console_tty, &tty_uart_ops, console_uart);
#else
    return 0;
#endif
}

static int uart_console_write(const char *buf, int len)
{
#ifdef CONFIG_TTY
    return nos_tty_write(&console_tty, (const unsigned char *)buf, (unsigned int)len);
#else
    return uart_write(console_uart, (const unsigned char *)buf, (unsigned int)len);
#endif
}

static int uart_console_write_record(const char *buf, int len)
{
    return uart_write_record(console_uart, (const unsigned char *)buf,
                             (unsigned int)len);
}

static int uart_console_write_recordv(const char *first, unsigned int first_len,
                                      const char *second, unsigned int second_len)
{
    return uart_write_recordv(console_uart, (const unsigned char *)first, first_len,
                              (const unsigned char *)second, second_len);
}

static int uart_console_getc(void)
{
    unsigned char ch;
#ifdef CONFIG_TTY
    int rc = nos_tty_read(&console_tty, &ch, 1, true);
#else
    int rc = uart_read(console_uart, &ch, 1, true);
#endif
    return rc < 0 ? rc : (int)ch;
}

static int uart_console_write_room(void)
{
    return uart_write_room(console_uart);
}

static int uart_console_putc(char ch)
{
    int rc = uart_write(console_uart, (const unsigned char *)&ch, 1);
    return rc == 1 ? 0 : rc;
}

static int uart_console_panic_begin(void)
{
    return uart_panic_begin(console_uart);
}

static int uart_console_panic_write(const char *buf, int len)
{
    return uart_panic_write(console_uart, (const unsigned char *)buf,
                            (unsigned int)len);
}

static struct console_ops uart_console_ops = {
    .init = uart_console_init,
    .write = uart_console_write,
    .write_record = uart_console_write_record,
    .write_recordv = uart_console_write_recordv,
    .write_room = uart_console_write_room,
    .getc = uart_console_getc,
    .putc = uart_console_putc,
    .panic_begin = uart_console_panic_begin,
    .panic_write = uart_console_panic_write,
};

console_register(tty0, &uart_console_ops);
