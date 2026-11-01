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
            rc = panic ? uart_panic_write(console_uart, buf + pos, end - pos) :
                         uart_write(console_uart, buf + pos, end - pos);
            if (rc <= 0) return pos != 0 ? (int)pos : rc;
            pos += (unsigned int)rc;
            if (pos != end) return (int)pos;
        }
        if (pos < len) {
            static const unsigned char crlf[] = { '\r', '\n' };

            /* Preserve the canonical TTY's explicit CRLF echo. */
            if (pos != 0 && buf[pos - 1] == '\r') {
                rc = panic ? uart_panic_write(console_uart, buf + pos, 1) :
                             uart_write(console_uart, buf + pos, 1);
                if (rc != 1) return pos != 0 ? (int)pos : rc;
            } else {
                rc = panic ? uart_panic_write(console_uart, crlf, sizeof(crlf)) :
                             uart_write_record(console_uart, crlf, sizeof(crlf));
                if (rc != (int)sizeof(crlf)) return pos != 0 ? (int)pos : (rc < 0 ? rc : -EIO);
            }
            pos++;
        }
    }
    return (int)pos;
}

static int uart_console_record_text(const char *first, unsigned int first_len,
                                    const char *second, unsigned int second_len)
{
    int rc;

    rc = uart_write_text_recordv(console_uart, (const unsigned char *)first, first_len,
                                 (const unsigned char *)second, second_len);
    if (rc != -ENOSYS) return rc;
    /* Polling-only UARTs have no queued record interface. */
    rc = uart_console_text_write((const unsigned char *)first, first_len, false);
    if (rc < 0 || (unsigned int)rc != first_len) return rc;
    if (second_len != 0) {
        int tail = uart_console_text_write((const unsigned char *)second, second_len, false);
        if (tail < 0) return rc != 0 ? rc : tail;
        rc += tail;
    }
    return rc;
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
#ifdef CONFIG_CONSOLE_CRLF
    (void)context;
    return uart_console_text_write(buf, len, false);
#else
    return uart_write(context, buf, len);
#endif
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
#ifdef CONFIG_CONSOLE_CRLF
    return uart_console_text_write((const unsigned char *)buf, (unsigned int)len, false);
#else
    return uart_write(console_uart, (const unsigned char *)buf, (unsigned int)len);
#endif
#endif
}

static int uart_console_write_record(const char *buf, int len)
{
#ifdef CONFIG_CONSOLE_CRLF
    return uart_console_record_text(buf, (unsigned int)len, NULL, 0);
#else
    return uart_write_record(console_uart, (const unsigned char *)buf,
                             (unsigned int)len);
#endif
}

static int uart_console_write_recordv(const char *first, unsigned int first_len,
                                      const char *second, unsigned int second_len)
{
#ifdef CONFIG_CONSOLE_CRLF
    return uart_console_record_text(first, first_len, second, second_len);
#else
    return uart_write_recordv(console_uart, (const unsigned char *)first, first_len,
                              (const unsigned char *)second, second_len);
#endif
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
#ifdef CONFIG_CONSOLE_CRLF
    int room = uart_write_room(console_uart);
    return room < 0 ? room : room / 2;
#else
    return uart_write_room(console_uart);
#endif
}

static int uart_console_putc(char ch)
{
#ifdef CONFIG_CONSOLE_CRLF
    int rc = uart_console_text_write((const unsigned char *)&ch, 1, false);
#else
    int rc = uart_write(console_uart, (const unsigned char *)&ch, 1);
#endif
    return rc == 1 ? 0 : rc;
}

static int uart_console_panic_begin(void)
{
    return uart_panic_begin(console_uart);
}

static int uart_console_panic_write(const char *buf, int len)
{
#ifdef CONFIG_CONSOLE_CRLF
    return uart_console_text_write((const unsigned char *)buf, (unsigned int)len, true);
#else
    return uart_panic_write(console_uart, (const unsigned char *)buf,
                            (unsigned int)len);
#endif
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
