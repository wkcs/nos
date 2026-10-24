/* Copyright (C) 2026 NOS contributors. */
#include "driver/uart_16550.h"
#define LSR_DR 1u
#define LSR_ERRORS 0x1eu
#define LSR_THRE 0x20u
static int configure(void *context, const struct nos_uart_config *c)
{
    struct nos_uart_16550 *p = context;
    uint32_t control, divisor, lsr;
    uint64_t denominator = (uint64_t)c->baud * 16u;
    if (c->rts_cts)
        return -NOS_DRV_ENOTSUP;
    /* Do not silently change a live stream's format or drop unsent bytes. */
    lsr = p->read(p->io_context, 5);
    if (!(lsr & 0x40u) || (lsr & LSR_DR))
        return -NOS_DRV_EBUSY;
    if (denominator > p->clock_hz)
        return -NOS_DRV_ENOTSUP;
    divisor = (uint32_t)(((uint64_t)p->clock_hz + denominator / 2u) / denominator);
    if (!divisor || divisor > 65535u)
        return -NOS_DRV_ENOTSUP;
    /* Reject baud error >3%; the caller must not be told an arbitrary clock
     * divisor achieved its requested rate. */
    {
        uint64_t expected = denominator * divisor;
        uint64_t delta = expected > p->clock_hz ? expected - p->clock_hz : p->clock_hz - expected;
        if (delta * 100u > expected * 3u)
            return -NOS_DRV_ENOTSUP;
    }
    control = c->data_bits - 5u;
    if (c->stop_bits == 2) {
        if (c->data_bits == 5)
            return -NOS_DRV_ENOTSUP; /* 16550 encodes 1.5 stops. */
        control |= 4;
    }
    if (c->parity)
        control |= c->parity == 1 ? 8u : 24u;
    p->write(p->io_context, 3, control); /* Clear inherited DLAB before IER. */
    p->write(p->io_context, 1, 0);       /* This polled backend does not own an IRQ. */
    p->write(p->io_context, 3, control | 0x80u);
    p->write(p->io_context, 0, divisor & 255u);
    p->write(p->io_context, 1, divisor >> 8);
    p->write(p->io_context, 3, control);
    return 0;
}
static intptr_t read_bytes(void *context, void *data, size_t count)
{
    struct nos_uart_16550 *p = context;
    uint8_t *bytes = data;
    size_t done = 0;
    while (done < count) {
        uint32_t status = p->read(p->io_context, 5);
        if (status & LSR_ERRORS) {
            if (status & LSR_DR)
                (void)p->read(p->io_context, 0);
            return done ? (intptr_t)done : -NOS_DRV_EIO;
        }
        if (!(status & LSR_DR))
            break;
        bytes[done++] = (uint8_t)p->read(p->io_context, 0);
    }
    return done ? (intptr_t)done : -NOS_DRV_EAGAIN;
}
static intptr_t write_bytes(void *context, const void *data, size_t count)
{
    struct nos_uart_16550 *p = context;
    const uint8_t *bytes = data;
    size_t done = 0;
    while (done < count && (p->read(p->io_context, 5) & LSR_THRE))
        p->write(p->io_context, 0, bytes[done++]);
    return done ? (intptr_t)done : -NOS_DRV_EAGAIN;
}
static int poll(void *context, uint32_t *events)
{
    struct nos_uart_16550 *p = context;
    uint32_t status = p->read(p->io_context, 5);
    *events = ((status & LSR_DR) ? NOS_UART_READABLE : 0) |
              ((status & LSR_THRE) ? NOS_UART_WRITABLE : 0) |
              ((status & LSR_ERRORS) ? NOS_UART_ERROR : 0);
    return 0;
}
static const struct nos_uart_ops ops = {configure, read_bytes, write_bytes, poll};
int nos_uart_16550_init(struct nos_uart_16550 *p, struct nos_device *device)
{
    if (!p || !device || !p->read || !p->write || !p->clock_hz)
        return -NOS_DRV_EINVAL;
    p->uart = (struct nos_uart){{device, p, 256, 0}, &ops};
    return 0;
}
