/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <drivers/serial.h>
#include <stdio.h>
#include <string.h>
#include "../../include/kernel/console.h"

static struct device test_device;
struct device *device_get_binding(const char *name) { (void)name; return &test_device; }
#include "../../drivers/serial/console_uart.c"

static unsigned char wire[256];
static unsigned int used;
static unsigned int room = sizeof(wire);

static ssize_t write_bytes(const struct device *dev, const unsigned char *buf, size_t len)
{
    (void)dev;
    if (len > room) return -EAGAIN;
    memcpy(wire + used, buf, len);
    used += len;
    room -= len;
    return (ssize_t)len;
}

static int poll_out_byte(const struct device *dev, unsigned char out)
{
    (void)dev;
    if (room < 1) return -EAGAIN;
    wire[used++] = out;
    room--;
    return 0;
}

static void panic_bytes(const struct device *dev, const unsigned char *buf, size_t len)
{
    (void)dev;
    if (len > room) len = room;
    memcpy(wire + used, buf, len);
    used += len;
    room -= len;
}

int main(void)
{
    static const struct uart_driver_api api = {
        .poll_out = poll_out_byte,
        .write = write_bytes,
        .panic_write = panic_bytes,
    };
    test_device.api = &api;
    test_device.ready = true;
    console_uart = &test_device;
#ifndef CONFIG_CONSOLE_CRLF
    if (uart_console_write("a\nb", 3) != 3 || used != 3 || memcmp(wire, "a\nb", 3)) {
        fputs("FAIL: disabling console conversion must preserve LF\n", stderr);
        return 1;
    }
    puts("console LF checks passed");
    return 0;
#else
    if (uart_console_write("a\nb", 3) != 3 || used != 4 || memcmp(wire, "a\r\nb", 4)) {
        fputs("FAIL: console stream must expand LF and return input length\n", stderr);
        return 1;
    }
    used = 0; room = 1;
    if (uart_console_write("\n", 1) != -EAGAIN || used != 0) {
        fputs("FAIL: a newline must not be split when the UART queue is full\n", stderr);
        return 1;
    }
    used = 0; room = sizeof(wire);
    if (uart_console_write("a\n", 2) != 2 || uart_console_write("b\n", 2) != 2 ||
        used != 6 || memcmp(wire, "a\r\nb\r\n", 6)) {
        fputs("FAIL: sequential writes must expand LF\n", stderr);
        return 1;
    }
    used = 0; room = sizeof(wire);
    if (uart_console_panic_write("x\n", 2) != 2 || used != 3 || memcmp(wire, "x\r\n", 3)) {
        fputs("FAIL: panic output must expand LF\n", stderr);
        return 1;
    }
    used = 0; room = sizeof(wire);
    if (uart_write(&test_device, (const unsigned char *)"x\n", 2) != 2 ||
        used != 2 || memcmp(wire, "x\n", 2)) {
        fputs("FAIL: raw UART must remain unchanged\n", stderr);
        return 1;
    }
    puts("console newline checks passed");
    return 0;
#endif
}
