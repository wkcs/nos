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

#ifdef CONFIG_CONSOLE_CRLF
/* Return the number of input bytes accepted, not the expanded wire size. */
static int uart_console_text_write(const unsigned char *buf, unsigned int len, bool panic)
{
    unsigned int pos = 0;

    while (pos < len) {
        unsigned int end = pos;
        int rc;

        while (end < len && buf[end] != '\n') end++;
        if (end != pos) {
            if (panic) {
                uart_panic_write(console_uart, buf + pos, end - pos);
                rc = (int)(end - pos);
            } else {
                rc = (int)uart_write(console_uart, buf + pos, end - pos);
            }
            if (rc <= 0) return pos != 0 ? (int)pos : rc;
            pos += (unsigned int)rc;
            if (pos != end) return (int)pos;
        }
        if (pos < len) {
            static const unsigned char crlf[] = { '\r', '\n' };

            /* Preserve the canonical TTY's explicit CRLF echo. */
            if (pos != 0 && buf[pos - 1] == '\r') {
                if (panic) {
                    uart_panic_write(console_uart, buf + pos, 1);
                    rc = 1;
                } else {
                    rc = (int)uart_write(console_uart, buf + pos, 1);
                }
                if (rc != 1) return pos != 0 ? (int)pos : rc;
            } else {
                if (panic) {
                    uart_panic_write(console_uart, crlf, sizeof(crlf));
                    rc = (int)sizeof(crlf);
                } else {
                    rc = (int)uart_write(console_uart, crlf, sizeof(crlf));
                }
                if (rc != (int)sizeof(crlf)) return pos != 0 ? (int)pos : (rc < 0 ? rc : -EIO);
            }
            pos++;
        }
    }
    return (int)pos;
}
#endif
#ifdef CONFIG_TTY
static struct nos_tty console_tty;

static int tty_uart_read(void *context, unsigned char *buf, unsigned int len, bool wait)
{
    return uart_read(context, buf, len, wait);
}

static int tty_uart_write(void *context, const unsigned char *buf, unsigned int len)
{
    /* TTY 线路规程已处理 ONLCR 模式，底层驱动按原始字节流直接输出 */
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
    int rc = nos_tty_init(&console_tty, &tty_uart_ops, console_uart);
    if (rc != 0) {
        return rc;
    }
#ifdef CONFIG_CONSOLE_CRLF
    console_tty.termios.c_oflag |= ONLCR;
#endif
    return 0;
#else
    return 0;
#endif
}

static int uart_console_write(const char *buf, int len)
{
#ifdef CONFIG_TTY
    return nos_tty_write(&console_tty, (const unsigned char *)buf, (unsigned int)len);
#else
#ifdef CONFIG_CONSOLE_CRLF
    return uart_console_text_write((const unsigned char *)buf, (unsigned int)len, false);
#else
    return (int)uart_write(console_uart, (const unsigned char *)buf, (size_t)len);
#endif
#endif
}

static int uart_console_getc(void)
{
    unsigned char ch;
#ifdef CONFIG_TTY
    int rc = nos_tty_read(&console_tty, &ch, 1, true);
#else
    int rc = (int)uart_read(console_uart, &ch, 1, true);
#endif
    return rc < 0 ? rc : (int)ch;
}

static int uart_console_write_room(void)
{
#ifdef CONFIG_LOG_MESSAGE_SIZE
    return CONFIG_LOG_MESSAGE_SIZE;
#else
    return 128;
#endif
}

static int uart_console_putc(char ch)
{
#ifdef CONFIG_CONSOLE_CRLF
    int rc = uart_console_text_write((const unsigned char *)&ch, 1, false);
#else
    int rc = (int)uart_write(console_uart, (const unsigned char *)&ch, 1);
#endif
    return rc == 1 ? 0 : rc;
}

static int uart_console_panic_begin(void)
{
    uart_panic_begin(console_uart);
    return 0;
}

static int uart_console_panic_write(const char *buf, int len)
{
#ifdef CONFIG_CONSOLE_CRLF
    return uart_console_text_write((const unsigned char *)buf, (unsigned int)len, true);
#else
    uart_panic_write(console_uart, (const unsigned char *)buf, (size_t)len);
    return len;
#endif
}

static struct console_ops uart_console_ops = {
    .init = uart_console_init,
    .write = uart_console_write,
    .write_room = uart_console_write_room,
    .getc = uart_console_getc,
    .putc = uart_console_putc,
    .panic_begin = uart_console_panic_begin,
    .panic_write = uart_console_panic_write,
};

console_register(tty0, &uart_console_ops);
