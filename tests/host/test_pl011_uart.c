/**
 * Copyright (C) 2026 Nick Hu
 *
 * Test PL011 early polling with simulated memory-mapped registers.
 */

#include <drivers/serial.h>
#include <stdio.h>
#if __has_include("../../drivers/serial/uart_pl011.c")
#include "../../drivers/serial/uart_pl011.c"
#define TEST_HAS_PL011 1
#endif

int main(void)
{
#ifndef TEST_HAS_PL011
    fputs("FAIL: PL011 UART driver is unavailable\n", stderr);
    return 1;
#else
    uint32_t regs[32] = { 0 };
    const struct uart_pl011_config config = {
        .base = (uintptr_t)regs,
        .clock_hz = 24000000,
        .baud_rate = 115200,
        .poll_timeout = 4,
    };
    struct device dev = { .config = &config, .api = &uart_pl011_api, .ready = true };
    unsigned char ch = 0xff;

    if (uart_pl011_init(&dev) != 0 || regs[PL011_IBRD / 4] != 13 ||
        regs[PL011_FBRD / 4] != 1) {
        fputs("FAIL: initialize PL011 baud divisor\n", stderr);
        return 1;
    }
    if (uart_poll_out(&dev, 'A') != 0 || regs[PL011_DR / 4] != 'A') {
        fputs("FAIL: polling output writes a byte\n", stderr);
        return 1;
    }
    regs[PL011_DR / 4] = 0;
    if (uart_read(&dev, &ch, 1, false) != 1 || ch != 0) {
        fputs("FAIL: polling input preserves NUL\n", stderr);
        return 1;
    }
    regs[PL011_FR / 4] = PL011_FR_RXFE | PL011_FR_TXFF;
    if (uart_poll_in(&dev, &ch) != -EAGAIN ||
        uart_read(&dev, &ch, 1, true) != -ETIMEDOUT ||
        uart_poll_out(&dev, 'B') != -ETIMEDOUT) {
        fputs("FAIL: PL011 empty/full polling is bounded\n", stderr);
        return 1;
    }
    puts("PL011 polling checks passed");
    return 0;
#endif
}
