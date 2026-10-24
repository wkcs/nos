/* Copyright (C) 2026 NOS contributors. */
#ifndef NOS_DRIVER_UART_16550_H
#define NOS_DRIVER_UART_16550_H
#include "peripheral.h"
/* Authorized register callbacks use a logical register index (0..7). The BSP
 * supplies register width/stride/barriers, so this driver assumes no CPU or bus
 * address. clock_hz is the actual UART input clock after clock-provider setup.
 * This is the base 16550 subset, not Rockchip's complete DW APB UART driver. */
struct nos_uart_16550 {
    struct nos_uart uart;
    void *io_context;
    uint32_t (*read)(void *, unsigned register_index);
    void (*write)(void *, unsigned register_index, uint32_t);
    uint32_t clock_hz;
};
/* Device must already be registered/bound by the BSP. Does not touch hardware
 * until configure; object and device must stay at their original addresses. */
int nos_uart_16550_init(struct nos_uart_16550 *, struct nos_device *);
#endif
