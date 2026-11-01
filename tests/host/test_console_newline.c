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
static int write_bytes(const struct device *dev, const unsigned char *buf, unsigned int len)
{
    (void)dev;
    if (len > room) return -EAGAIN;
    memcpy(wire + used, buf, len);
    used += len;
    room -= len;
    return (int)len;
}
static int write_recordv(const struct device *dev, const unsigned char *a, unsigned int alen,
                         const unsigned char *b, unsigned int blen)
{
    if (alen + blen > room) return -EAGAIN;
    if (alen) write_bytes(dev, a, alen);
    if (blen) write_bytes(dev, b, blen);
    return (int)(alen + blen);
}
static int write_room(const struct device *dev) { (void)dev; return (int)room; }
static int write_text_recordv(const struct device *dev, const unsigned char *a, unsigned int alen,
                              const unsigned char *b, unsigned int blen)
{
    unsigned char out[64];
    unsigned int i, n = 0;
    unsigned char prev = 0;
    for (i = 0; i < alen + blen; i++) {
        unsigned char ch = i < alen ? a[i] : b[i - alen];
        if (ch == '\n' && prev != '\r') out[n++] = '\r';
        out[n++] = ch;
        prev = ch;
    }
    if (write_bytes(dev, out, n) < 0) return -EAGAIN;
    return (int)(alen + blen);
}

int main(void)
{
    static const struct uart_driver_api api = {
        .write = write_bytes, .write_record = write_bytes,
        .write_recordv = write_recordv, .write_room = write_room,
        .write_text_recordv = write_text_recordv,
        .panic_write = write_bytes,
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
    if (uart_console_write("\n", 1) != -EAGAIN || used != 0 ||
        uart_console_write_room() != 0) {
        fputs("FAIL: a newline must not be split when the UART queue is full\n", stderr);
        return 1;
    }
    used = 0; room = 3;
    if (uart_console_write_recordv("a\n", 2, "b\n", 2) != -EAGAIN || used != 0) {
        fputs("FAIL: expanded record must be atomic under backpressure\n", stderr);
        return 1;
    }
    room = sizeof(wire);
    if (uart_console_write_recordv("a\n", 2, "b\n", 2) != 4 || used != 6 ||
        memcmp(wire, "a\r\nb\r\n", 6)) {
        fputs("FAIL: split record must expand LF\n", stderr);
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
