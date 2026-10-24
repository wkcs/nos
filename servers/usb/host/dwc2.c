/* Copyright (C) 2026 NOS contributors. */
#include "driver/usb_dwc2.h"
#define GAHBCFG 0x008u
#define GUSBCFG 0x00cu
#define GRSTCTL 0x010u
#define GINTSTS 0x014u
#define GINTMSK 0x018u
#define GRXFSIZ 0x024u
#define GNPTXFSIZ 0x028u
#define GSNPSID 0x040u
#define GHWCFG2 0x048u
#define GHWCFG3 0x04cu
#define HPTXFSIZ 0x100u
#define HCFG 0x400u
#define HFIR 0x404u
#define HFNUM 0x408u
#define HAINTMSK 0x418u
#define HPRT 0x440u
#define HCCHAR 0x500u
#define HCSPLT 0x504u
#define HCINT 0x508u
#define HCINTMSK 0x50cu
#define HCTSIZ 0x510u
#define HCDMA 0x514u
#define CHENA (1u << 31)
#define CHDIS (1u << 30)
#define HALTED (1u << 1)
#define COMPLETE 1u
#define HPRT_CHANGES ((1u << 1) | (1u << 3) | (1u << 5))
static uint32_t read_reg(struct nos_usb_dwc2 *d, uint32_t r)
{
    return d->io.read32(d->io.context, r);
}
static void write_reg(struct nos_usb_dwc2 *d, uint32_t r, uint32_t v)
{
    d->io.write32(d->io.context, r, v);
}
static int wait_reg(struct nos_usb_dwc2 *d, uint32_t r, uint32_t mask, uint32_t value,
                    uint32_t timeout)
{
    uint32_t start = d->io.time_us(d->io.context);
    unsigned i;
    for (i = 0; i < 100000; i++) {
        if ((read_reg(d, r) & mask) == value)
            return 0;
        if ((uint32_t)(d->io.time_us(d->io.context) - start) >= timeout)
            break;
        d->io.delay_us(d->io.context, 10);
    }
    return -NOS_DRV_ETIMEDOUT;
}
static uint32_t port_value(struct nos_usb_dwc2 *d)
{
    return read_reg(d, HPRT) & ~(HPRT_CHANGES | (1u << 2));
}
static int io_valid(struct nos_usb_dwc2 *d)
{
    return d && d->io.read32 && d->io.write32 && d->io.time_us && d->io.delay_us && d->io.dma_map &&
           d->io.dma_unmap && d->io.dma_sync;
}
static int reset_core(struct nos_usb_dwc2 *d)
{
    int error = wait_reg(d, GRSTCTL, 1u << 31, 1u << 31, 10000);
    if (error)
        return error;
    write_reg(d, GRSTCTL, 1);
    error = wait_reg(d, GRSTCTL, 1, 0, 10000);
    d->io.delay_us(d->io.context, 10);
    return error;
}
static int start(void *context)
{
    struct nos_usb_dwc2 *d = context;
    uint32_t id, hw, depth, rx_words, tx_words;
    int error;
    if (!io_valid(d))
        return -NOS_DRV_EINVAL;
    if (d->mapped || d->started)
        return -NOS_DRV_EBUSY;
    id = read_reg(d, GSNPSID);
    hw = read_reg(d, GHWCFG2);
    if ((id & 0xffff0000u) != 0x4f540000u || ((hw >> 3) & 3) != 2 || (hw & 7) == 3 || (hw & 7) == 4)
        return -NOS_DRV_ENOTSUP; /* This backend requires internal DMA. */
    depth = read_reg(d, GHWCFG3) >> 16;
    if (depth < 96)
        return -NOS_DRV_ENOSPC;
    if (d->io.phy_clock_hz && d->io.phy_clock_hz != 30000000 && d->io.phy_clock_hz != 48000000 &&
        d->io.phy_clock_hz != 60000000)
        return -NOS_DRV_EINVAL;
    d->max_packet = depth >= 608 ? 512 : 64;
    rx_words = depth >= 608 ? 288 : 48;
    tx_words = depth >= 608 ? 160 : 24;
    write_reg(d, GINTMSK, 0);
    write_reg(d, GAHBCFG, 0);
    error = reset_core(d);
    if (error)
        return error;
    write_reg(d, GUSBCFG, (read_reg(d, GUSBCFG) & ~(1u << 30)) | (1u << 29));
    d->io.delay_us(d->io.context, 25000);
    error = wait_reg(d, GINTSTS, 1, 1, 10000);
    if (error)
        return error;
    error = nos_driver_status(
        d->io.dma_map(d->io.context, d->bounce, sizeof(d->bounce), &d->dma_address));
    if (error)
        return error;
    d->mapped = 1;
    if ((d->dma_address & 3) || d->dma_address > UINT32_MAX - sizeof(d->bounce))
        return -NOS_DRV_EINVAL;
    write_reg(d, GRXFSIZ, rx_words);
    write_reg(d, GNPTXFSIZ, (tx_words << 16) | rx_words);
    write_reg(d, HPTXFSIZ, (tx_words << 16) | (rx_words + tx_words));
    write_reg(d, GRSTCTL, (16u << 6) | (1u << 5));
    error = wait_reg(d, GRSTCTL, 1u << 5, 0, 10000);
    if (error)
        return error;
    write_reg(d, GRSTCTL, 1u << 4);
    error = wait_reg(d, GRSTCTL, 1u << 4, 0, 10000);
    if (error)
        return error;
    write_reg(d, HCFG, 0); /* 30/60 MHz PHY clock for HS; reset selects FS/LS below. */
    write_reg(d, HAINTMSK, 0);
    write_reg(d, HCINTMSK, 0);
    write_reg(d, HCINT, 0x3fff);
    write_reg(d, GAHBCFG, (1u << 5) | (3u << 1)); /* DMA, INCR4, polled (global IRQ disabled). */
    write_reg(d, HPRT, port_value(d) | (1u << 12));
    d->started = 1;
    d->faulted = 0;
    return 0;
}
static int stop(void *context)
{
    struct nos_usb_dwc2 *d = context;
    int error;
    if (!io_valid(d))
        return -NOS_DRV_EINVAL;
    if (!d->mapped && !d->started)
        return 0;
    write_reg(d, GINTMSK, 0);
    write_reg(d, HCINTMSK, 0);
    /* Reset, rather than assume clearing DMA-enable cancels a bus master. */
    error = reset_core(d);
    if (error) {
        d->faulted = 1;
        return error;
    }
    write_reg(d, GAHBCFG, 0);
    write_reg(d, HPRT, port_value(d) & ~(1u << 12));
    if (d->mapped) {
        d->io.dma_sync(d->io.context, d->bounce, sizeof(d->bounce), 0);
        d->io.dma_unmap(d->io.context, d->bounce, sizeof(d->bounce), d->dma_address);
    }
    d->mapped = d->started = d->faulted = 0;
    return 0;
}
static int status(void *context, uint32_t *flags)
{
    struct nos_usb_dwc2 *d = context;
    uint32_t p;
    if (!d || !d->started)
        return -NOS_DRV_ENODEV;
    p = read_reg(d, HPRT);
    *flags = ((p & 1) ? NOS_USB_PORT_CONNECTED : 0) | ((p & 2) ? NOS_USB_PORT_CHANGED : 0) |
             ((p & (1u << 4)) ? NOS_USB_PORT_OVERCURRENT : 0) |
             ((p & 4) ? NOS_USB_PORT_ENABLED : 0);
    if (p & HPRT_CHANGES)
        write_reg(d, HPRT, port_value(d) | (p & HPRT_CHANGES));
    return 0;
}
static int port_reset(void *context, uint8_t *speed)
{
    struct nos_usb_dwc2 *d = context;
    uint32_t p, code;
    if (!d || !d->started || d->faulted)
        return -NOS_DRV_ENODEV;
    p = read_reg(d, HPRT);
    if (!(p & 1) || (p & (1u << 4)))
        return -NOS_DRV_ENODEV;
    write_reg(d, HPRT, port_value(d) | (1u << 8));
    d->io.delay_us(d->io.context, 50000);
    write_reg(d, HPRT, port_value(d) & ~(1u << 8));
    d->io.delay_us(d->io.context, 10000);
    p = read_reg(d, HPRT);
    if (!(p & 1) || !(p & 4) || (p & (1u << 4)))
        return -NOS_DRV_ENODEV;
    code = (p >> 17) & 3;
    if (code > 2)
        return -NOS_DRV_ENOTSUP;
    *speed = code == 0 ? NOS_USB_HIGH : code == 1 ? NOS_USB_FULL : NOS_USB_LOW;
    {
        uint32_t clock = d->io.phy_clock_hz;
        if (!clock)
            clock = read_reg(d, GUSBCFG) & (1u << 6) ? 48000000 : 60000000;
        write_reg(d, HCFG, code == 2 ? 2 : clock == 48000000 ? 1 : 0);
        write_reg(d, HFIR, code == 2 ? 6000 : clock / (code == 0 ? 8000 : 1000));
    }
    return 0;
}
static int halt_channel(struct nos_usb_dwc2 *d)
{
    uint32_t c = read_reg(d, HCCHAR);
    if (!(c & CHENA))
        return 0;
    write_reg(d, HCCHAR, c | CHDIS | CHENA);
    if (wait_reg(d, HCCHAR, CHENA, 0, 10000)) {
        d->faulted = 1;
        return -NOS_DRV_ETIMEDOUT;
    }
    return 0;
}
static int packet(void *context, const struct nos_usb_packet *p, size_t *actual, uint32_t timeout)
{
    struct nos_usb_dwc2 *d = context;
    uint32_t size, pid, chars, start, bits = 0;
    unsigned loops, i;
    int error = 0;
    if (!actual)
        return -NOS_DRV_EINVAL;
    *actual = 0;
    if (!d || !d->started || !d->mapped)
        return -NOS_DRV_ENODEV;
    if (d->faulted)
        return -NOS_DRV_EIO;
    if (!p || p->address > 127 || (p->endpoint & 0x70) || p->type == NOS_USB_ISOCHRONOUS ||
        p->type > NOS_USB_INTERRUPT || p->pid > NOS_USB_SETUP || !p->max_packet ||
        p->max_packet > sizeof(d->bounce) || p->length > p->max_packet || (p->length && !p->data) ||
        !timeout || timeout > INT32_MAX || p->speed < NOS_USB_LOW || p->speed > NOS_USB_HIGH ||
        (p->pid == NOS_USB_SETUP && (p->endpoint || p->length != 8 || p->type != NOS_USB_CONTROL)))
        return -NOS_DRV_EINVAL;
    if (p->max_packet > d->max_packet)
        return -NOS_DRV_ENOTSUP;
    if (read_reg(d, HCCHAR) & CHENA)
        return -NOS_DRV_EBUSY;
    size = p->endpoint & 0x80 ? p->max_packet : (uint32_t)p->length;
    for (i = 0; i < sizeof(d->bounce); i++)
        d->bounce[i] = 0;
    if (!(p->endpoint & 0x80))
        for (i = 0; i < p->length; i++)
            d->bounce[i] = ((const uint8_t *)p->data)[i];
    d->io.dma_sync(d->io.context, d->bounce, sizeof(d->bounce), 1);
    pid = p->pid == NOS_USB_SETUP ? 3 : p->pid == NOS_USB_DATA1 ? 2 : 0;
    write_reg(d, HCINT, 0x3fff);
    write_reg(d, HCSPLT, 0);
    write_reg(d, HCTSIZ, size | (1u << 19) | (pid << 29));
    write_reg(d, HCDMA, d->dma_address);
    chars = p->max_packet | ((uint32_t)(p->endpoint & 15) << 11) |
            ((p->endpoint & 0x80) ? 1u << 15 : 0) | (p->speed == NOS_USB_LOW ? 1u << 17 : 0) |
            ((uint32_t)p->type << 18) | (1u << 20) | ((uint32_t)p->address << 22);
    if (p->type == NOS_USB_INTERRUPT && !(read_reg(d, HFNUM) & 1))
        chars |= 1u << 29;
    start = d->io.time_us(d->io.context);
    write_reg(d, HCCHAR, chars | CHENA);
    for (loops = 0; loops < 100000; loops++) {
        uint32_t prt = read_reg(d, HPRT);
        bits = read_reg(d, HCINT);
        if (!(prt & 1) || (prt & (1u << 4))) {
            error = -NOS_DRV_ENODEV;
            break;
        }
        if (bits & HALTED)
            break;
        if (bits &
            ((1u << 4) | (1u << 3) | (1u << 2) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 10)))
            break;
        if ((uint32_t)(d->io.time_us(d->io.context) - start) >= timeout)
            break;
        d->io.delay_us(d->io.context, 10);
    }
    if (!error) {
        if (bits & (1u << 3))
            error = -NOS_USB_EPIPE;
        else if (bits & (1u << 4))
            error = -NOS_DRV_EAGAIN;
        else if (bits & ((1u << 2) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 10)))
            error = -NOS_DRV_EIO;
        else if (!(bits & COMPLETE))
            error = -NOS_DRV_ETIMEDOUT;
    }
    /* NAK may leave a DMA channel scheduled; always confirm it has stopped. */
    if (halt_channel(d))
        return -NOS_DRV_EIO;
    d->io.dma_sync(d->io.context, d->bounce, sizeof(d->bounce), 0);
    if (!error) {
        uint32_t left = read_reg(d, HCTSIZ) & 0x7ffffu;
        if (left > size || size - left > p->length)
            error = -NOS_DRV_EIO;
        else {
            *actual = size - left;
            if (p->endpoint & 0x80)
                for (i = 0; i < *actual; i++)
                    ((uint8_t *)p->data)[i] = d->bounce[i];
        }
    }
    write_reg(d, HCINT, 0x3fff);
    return error;
}
static uint32_t now(void *context)
{
    struct nos_usb_dwc2 *d = context;
    return d->io.time_us(d->io.context);
}
static void delay(void *context, uint32_t us)
{
    struct nos_usb_dwc2 *d = context;
    d->io.delay_us(d->io.context, us);
}
const struct nos_usb_host_ops nos_usb_dwc2_host_ops = {start,  stop, status, port_reset,
                                                       packet, now,  delay};
