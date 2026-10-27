/**
 * Copyright (C) 2026 Nick Hu
 *
 * Serial wrapper error and byte-value contract.
 */

#include <drivers/serial.h>
#include <stdio.h>

static int poll_in(const struct device *dev, unsigned char *value)
{
    (void)dev;
    *value = 0;
    return 0;
}

#ifdef NOS_UART_BULK_API
static int write_bytes(const struct device *dev, const unsigned char *buf,
                       unsigned int len)
{
    (void)dev;
    (void)buf;
    return len > 2 ? 2 : (int)len;
}
#endif

int main(void)
{
    const struct uart_driver_api api = {
        .poll_in = poll_in,
#ifdef NOS_UART_BULK_API
        .write = write_bytes,
#endif
    };
    const struct device dev = { .api = &api, .ready = true };
    unsigned char value = 255;

    if (uart_poll_in(&dev, NULL) != -EINVAL) {
        fputs("FAIL: null input destination must report EINVAL\n", stderr);
        return 1;
    }
    if (uart_poll_in(&dev, &value) != 0 || value != 0) {
        fputs("FAIL: NUL is a valid input byte\n", stderr);
        return 1;
    }
    if (uart_poll_out(&dev, 'x') != -ENOSYS) {
        fputs("FAIL: absent operation must report ENOSYS\n", stderr);
        return 1;
    }
#ifdef NOS_UART_BULK_API
    if (uart_write(&dev, (const unsigned char *)"abc", 3) != 2 ||
        uart_write(&dev, NULL, 1) != -EINVAL) {
        fputs("FAIL: bulk output reports actual bytes and validates input\n", stderr);
        return 1;
    }
#else
    fputs("FAIL: bulk UART interface is unavailable\n", stderr);
    return 1;
#endif
    puts("serial API checks passed");
    return 0;
}
