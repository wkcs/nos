/* Copyright (C) 2026 NOS contributors. */
#include "nk60_usb.h"
#define EINVAL 22
#define EIO 5
#define EAGAIN 11
#define ETIMEDOUT 110
#define GAHBCFG 0x008u
#define GUSBCFG 0x00cu
#define GRSTCTL 0x010u
#define GINTSTS 0x014u
#define GINTMSK 0x018u
#define GRXSTSP 0x020u
#define GRXFSIZ 0x024u
#define TX0SIZE 0x028u
#define GCCFG 0x038u
#define TX1SIZE 0x104u
#define TX2SIZE 0x108u
#define DCFG 0x800u
#define DCTL 0x804u
#define DIEPMSK 0x810u
#define DOEPMSK 0x814u
#define DAINT 0x818u
#define DAINTMSK 0x81cu
#define DIEPEMPMSK 0x834u
#define INCTL(n) (0x900u + 0x20u * (n))
#define ININT(n) (0x908u + 0x20u * (n))
#define INSIZE(n) (0x910u + 0x20u * (n))
#define INFSPACE(n) (0x918u + 0x20u * (n))
#define OUTCTL 0xb00u
#define OUTINT 0xb08u
#define OUTSIZE 0xb10u
#define FIFO(n) (0x1000u + 0x1000u * (n))
#define RXFLVL (1u << 4)
#define USBRST (1u << 12)
#define ENUMDNE (1u << 13)
#define IEPINT (1u << 18)
#define OEPINT (1u << 19)
#define USBSUSP (1u << 11)
#define WKUPINT (1u << 31)
#define EPENA (1u << 31)
#define EPDIS (1u << 30)
#define SNAK (1u << 27)
#define CNAK (1u << 26)
#define STALL (1u << 21)
#define USBAEP (1u << 15)
#define SD0PID (1u << 28)
static uint32_t read_reg(struct nk60_usb *d, unsigned offset)
{
    return d->io.read32(d->io.context, NK60_USB_BASE + offset);
}
static void write_reg(struct nk60_usb *d, unsigned offset, uint32_t value)
{
    d->io.write32(d->io.context, NK60_USB_BASE + offset, value);
}
static void update_reg(struct nk60_usb *d, unsigned offset, uint32_t clear, uint32_t set)
{
    write_reg(d, offset, (read_reg(d, offset) & ~clear) | set);
}
static int wait_bits(struct nk60_usb *d, unsigned offset, uint32_t mask, uint32_t value)
{
    uint32_t start = d->io.time_us(d->io.context), i;
    for (i = 0; i < 100000u; ++i) {
        if ((read_reg(d, offset) & mask) == value)
            return 0;
        if ((uint32_t)(d->io.time_us(d->io.context) - start) >= 10000u)
            break;
    }
    return -ETIMEDOUT;
}
static int flush_fifo(struct nk60_usb *d, unsigned number)
{
    write_reg(d, GRSTCTL, (1u << 5) | (number << 6));
    if (wait_bits(d, GRSTCTL, 1u << 5, 0))
        return -ETIMEDOUT;
    d->io.delay_us(d->io.context, 3);
    return 0;
}
static void fail_device(struct nk60_usb *d, int error)
{
    d->error = error;
    d->ready = 0;
    update_reg(d, DCTL, 1u, 2u);
    write_reg(d, GINTMSK, 0);
    write_reg(d, GAHBCFG, 0);
}
static void arm_out(struct nk60_usb *d)
{
    /* Three back-to-back SETUP packets and one max-size control OUT packet. */
    write_reg(d, OUTSIZE, (3u << 29) | (1u << 19) | 64u);
    update_reg(d, OUTCTL, EPDIS, USBAEP | CNAK | EPENA);
}
static void arm_nlp_out(struct nk60_usb *d)
{
    if (!d->keyboard.configured || d->nlp_ready || (d->keyboard.nlp_halted & 1))
        return;
    write_reg(d, OUTSIZE + 0x40, (1u << 19) | 64u);
    update_reg(d, OUTCTL + 0x40, EPDIS, USBAEP | CNAK | EPENA);
}
static int controller_send(void *context, uint8_t endpoint, const void *data, size_t length)
{
    struct nk60_usb *d = context;
    const uint8_t *bytes = data;
    unsigned ep = endpoint & 15u, i, j;
    uint32_t word;
    if (!d->ready)
        return -EIO;
    if (!(endpoint & 0x80) || ep > 2 || length > 64)
        return -EINVAL;
    if (ep == 2 &&
        (!d->keyboard.configured || d->keyboard.suspended || (d->keyboard.nlp_halted & 2)))
        return -EAGAIN;
    if (d->tx_busy[ep] || (read_reg(d, INFSPACE(ep)) & 0xffffu) < (length + 3) / 4)
        return -EAGAIN;
    d->tx_busy[ep] = 1;
    write_reg(d, ININT(ep), 1u); /* discard stale transfer-complete */
    write_reg(d, INSIZE(ep), (1u << 19) | (uint32_t)length);
    update_reg(d, INCTL(ep), EPDIS, CNAK | EPENA);
    for (i = 0; i < length; i += 4) {
        word = 0;
        for (j = 0; j < 4 && i + j < length; ++j)
            word |= (uint32_t)bytes[i + j] << (j * 8);
        write_reg(d, FIFO(ep), word);
    }
    return 0;
}
static void cancel(void *context, uint8_t endpoint)
{
    struct nk60_usb *d = context;
    unsigned ep = endpoint & 15u;
    uint32_t ctl;
    if (!(endpoint & 0x80) || ep > 2)
        return;
    /* A fresh SETUP aborts EP0 in hardware. For EP1 explicitly disable the
     * endpoint before flushing FIFO, so a partial packet cannot escape later. */
    ctl = read_reg(d, INCTL(ep));
    if (ep && (ctl & EPENA)) {
        write_reg(d, INCTL(ep), ctl | SNAK | EPDIS);
        if (wait_bits(d, INCTL(ep), EPENA, 0)) {
            fail_device(d, -ETIMEDOUT);
            return;
        }
    }
    if (flush_fifo(d, ep)) {
        fail_device(d, -ETIMEDOUT);
        return;
    }
    write_reg(d, ININT(ep), 0xffffffffu);
    d->tx_busy[ep] = 0;
}
static void stall(void *context, uint8_t endpoint, int enabled)
{
    struct nk60_usb *d = context;
    unsigned ep = endpoint & 15u, reg;
    if (ep > 2 || (!(endpoint & 0x80) && ep != 0 && ep != 2))
        return;
    reg = endpoint & 0x80 ? INCTL(ep) : OUTCTL + ep * 0x20;
    if (enabled)
        update_reg(d, reg, 0, STALL);
    else {
        update_reg(d, reg, STALL, ep ? SD0PID : 0);
        if (endpoint == 2)
            arm_nlp_out(d);
    }
}
static void configure(void *context, int enabled)
{
    struct nk60_usb *d = context;
    if (enabled != 3) {
        ++d->nlp_generation;
        d->nlp_ready = d->nlp_length = 0;
        if (d->ready)
            cancel(d, 0x82);
        write_reg(d, ININT(2), 0xffffffffu);
        write_reg(d, OUTINT + 0x40, 0xffffffffu);
    }
    if (enabled) {
        if (enabled != 2) {
            write_reg(d, ININT(1), 0xffffffffu);
            write_reg(d, INCTL(1), 64u | USBAEP | (3u << 18) | (1u << 22) | SD0PID | SNAK);
            update_reg(d, DAINTMSK, 0, 2u);
        }
        if (enabled == 3 || !d->keyboard.nlp_enabled)
            return;
        write_reg(d, INCTL(2), 64u | USBAEP | (2u << 18) | (2u << 22) | SD0PID | SNAK);
        write_reg(d, OUTCTL + 0x40, 64u | USBAEP | (2u << 18) | SD0PID | SNAK);
        update_reg(d, DAINTMSK, 0, 6u | (1u << 18));
        arm_nlp_out(d);
    } else {
        update_reg(d, DAINTMSK, 6u | (1u << 18), 0);
        update_reg(d, INCTL(2), USBAEP | STALL, SNAK);
        update_reg(d, OUTCTL + 0x40, USBAEP | STALL, SNAK);
        d->tx_busy[2] = 0;
        update_reg(d, INCTL(1), USBAEP | STALL, SNAK);
        d->tx_busy[1] = 0;
    }
}
static void address(void *context, uint8_t address_value)
{
    struct nk60_usb *d = context;
    update_reg(d, DCFG, 127u << 4, (uint32_t)address_value << 4);
}
static void wake(void *context, int asserted)
{
    struct nk60_usb *d = context;
    update_reg(d, DCTL, 1u, asserted ? 1u : 0);
    if (asserted) {
        /* The hardware pulse is bounded even if the caller stalls its event
         * loop afterwards: USB requires remote resume between 1 and 15ms. */
        d->io.delay_us(d->io.context, 2000);
        update_reg(d, DCTL, 1u, 0);
    }
}
static void open_control(struct nk60_usb *d)
{
    write_reg(d, INCTL(0), USBAEP | SNAK); /* MPS=64 encoding 0 */
    write_reg(d, OUTCTL, USBAEP | SNAK);
    write_reg(d, ININT(0), 0xffffffffu);
    write_reg(d, OUTINT, 0xffffffffu);
    write_reg(d, DAINTMSK, 1u | (1u << 16));
    arm_out(d);
}
static void board_usb_pins(struct nk60_usb *d)
{
    const struct nk60_io *io = &d->io;
    uint32_t value;
    value = io->read32(io->context, NK60_RCC + 0x30u);
    io->write32(io->context, NK60_RCC + 0x30u, value | 1u);
    (void)io->read32(io->context, NK60_RCC + 0x30u);
    value = io->read32(io->context, NK60_RCC + 0x34u);
    io->write32(io->context, NK60_RCC + 0x34u, value | (1u << 7));
    (void)io->read32(io->context, NK60_RCC + 0x34u);
    /* Explicit disconnect before taking ownership of the data pins. */
    value = io->read32(io->context, NK60_GPIO_A);
    value = (value & ~((3u << 22) | (3u << 24))) | (1u << 22) | (1u << 24);
    io->write32(io->context, NK60_GPIO_A + 0x18u, (3u << 11) << 16);
    io->write32(io->context, NK60_GPIO_A, value);
    io->delay_us(io->context, 200);
    value = io->read32(io->context, NK60_GPIO_A + 4u);
    io->write32(io->context, NK60_GPIO_A + 4u, value & ~(3u << 11));
    value = io->read32(io->context, NK60_GPIO_A + 8u);
    io->write32(io->context, NK60_GPIO_A + 8u, value | (3u << 22) | (3u << 24));
    value = io->read32(io->context, NK60_GPIO_A + 12u);
    io->write32(io->context, NK60_GPIO_A + 12u, value & ~((3u << 22) | (3u << 24)));
    value = io->read32(io->context, NK60_GPIO_A + 0x24u);
    io->write32(io->context, NK60_GPIO_A + 0x24u,
                (value & ~((15u << 12) | (15u << 16))) | (10u << 12) | (10u << 16));
    value = io->read32(io->context, NK60_GPIO_A);
    io->write32(io->context, NK60_GPIO_A,
                (value & ~((3u << 22) | (3u << 24))) | (2u << 22) | (2u << 24));
}
static int controller_cancel(void *context, uint8_t ep)
{
    struct nk60_usb *d = context;
    if ((ep & 15) > 2)
        return -EINVAL;
    cancel(d, ep);
    return d->ready ? 0 : -EIO;
}
static int controller_halt(void *context, uint8_t ep, int enabled)
{
    if ((ep & 15) > 2)
        return -EINVAL;
    stall(context, ep, enabled);
    return 0;
}
static const struct nos_usb_device_ops controller_ops = {controller_send, controller_cancel,
                                                         controller_halt};
static int controller_probe(struct nos_device *dev)
{
    struct nk60_usb *d = dev->context;
    return d->ready ? 0 : -EIO;
}
static void controller_remove(struct nos_device *dev)
{
    nk60_usb_disconnect(dev->context);
}
static const struct nos_driver controller_driver = {"snps,dwc2-nk60", controller_probe,
                                                    controller_remove, NULL, NULL};
static int transmit(void *context, uint8_t ep, const uint8_t *data, size_t length)
{
    struct nk60_usb *d = context;
    return nos_usb_device_send(&d->controller, ep, data, length);
}
int nk60_usb_init(struct nk60_usb *d, const struct nk60_io *io)
{
    struct usb_keyboard_transport t;
    unsigned i;
    uint8_t *bytes = (uint8_t *)d;
    if (!d || !io || !io->read32 || !io->write32 || !io->time_us || !io->delay_us)
        return -EINVAL;
    for (i = 0; i < sizeof(*d); ++i)
        bytes[i] = 0;
    d->io = *io;
    t.context = d;
    t.send = transmit;
    t.cancel = cancel;
    t.stall = stall;
    t.configure = configure;
    t.address = address;
    t.wake = wake;
    usb_keyboard_init(&d->keyboard, &t);
    usb_keyboard_enable_nlp(&d->keyboard, 1);
    board_usb_pins(d);
    write_reg(d, GAHBCFG, 0);
    update_reg(d, DCTL, 0, 2u);
    /* Internal FS PHY, forced device mode; TRDT=6 for AHB84MHz. */
    update_reg(d, GUSBCFG, (1u << 29) | (15u << 10), (1u << 6) | (1u << 30) | (6u << 10));
    d->io.delay_us(d->io.context, 25000);
    if (wait_bits(d, GRSTCTL, 1u << 31, 1u << 31)) {
        fail_device(d, -ETIMEDOUT);
        return d->error;
    }
    write_reg(d, GRSTCTL, 1u);
    if (wait_bits(d, GRSTCTL, 1u, 0)) {
        fail_device(d, -ETIMEDOUT);
        return d->error;
    }
    d->io.delay_us(d->io.context, 3);
    /* No VBUS sense pin: PA9 is UART TX on this board. NOVBUSSENS=1. */
    update_reg(d, GCCFG, (1u << 18) | (1u << 19), (1u << 16) | (1u << 21));
    update_reg(d, DCFG, 3u | (127u << 4), 3u);
    write_reg(d, GRXFSIZ, 128);
    write_reg(d, TX0SIZE, (64u << 16) | 128u);
    write_reg(d, TX1SIZE, (64u << 16) | 192u);
    write_reg(d, TX2SIZE, (64u << 16) | 256u);
    if (flush_fifo(d, 16)) {
        fail_device(d, -ETIMEDOUT);
        return d->error;
    }
    write_reg(d, GRSTCTL, 1u << 4);
    if (wait_bits(d, GRSTCTL, 1u << 4, 0)) {
        fail_device(d, -ETIMEDOUT);
        return d->error;
    }
    write_reg(d, DIEPEMPMSK, 0);
    write_reg(d, DIEPMSK, 3u);
    write_reg(d, DOEPMSK, 9u);
    write_reg(d, GINTSTS, 0xffffffffu);
    open_control(d);
    write_reg(d, GINTMSK, RXFLVL | USBRST | ENUMDNE | IEPINT | OEPINT | USBSUSP | WKUPINT);
    d->ready = 1;
    write_reg(d, GAHBCFG, 1);
    update_reg(d, DCTL, 2u, 0);
    {
        struct nos_device *entries[1];
        struct nos_device_registry registry = {entries, 1, 0};
        int error;
        d->controller_device =
            (struct nos_device){.name = "usb-fs", .compatible = "snps,dwc2-nk60", .context = d};
        d->controller = (struct nos_usb_device_controller){
            {&d->controller_device, d, 64, 0}, &controller_ops, 7, 64};
        error = nos_device_register(&registry, &d->controller_device);
        if (!error)
            error = nos_device_bind(&d->controller_device, &controller_driver);
        if (error) {
            fail_device(d, error);
            return error;
        }
    }
    return 0;
}
static void receive_fifo(struct nk60_usb *d)
{
    uint32_t status, word;
    unsigned endpoint, length, packet, i, j;
    uint8_t data[64];
    update_reg(d, GINTMSK, RXFLVL, 0);
    status = read_reg(d, GRXSTSP);
    endpoint = status & 15u;
    length = (status >> 4) & 0x7ffu;
    packet = (status >> 17) & 15u;
    for (i = 0; i < length; i += 4) {
        word = read_reg(d, FIFO(0));
        for (j = 0; j < 4 && i + j < length && i + j < sizeof(data); ++j)
            data[i + j] = (uint8_t)(word >> (j * 8));
    }
    if (endpoint == 2 && packet == 2 && length <= sizeof(data) && d->keyboard.configured &&
        !d->nlp_ready) {
        for (i = 0; i < length; ++i)
            d->nlp_out[i] = data[i];
        d->nlp_length = (uint8_t)length;
        d->nlp_ready = 1;
    } else if (endpoint || length > sizeof(data)) {
        if (!endpoint) {
            stall(d, 0, 1);
            stall(d, 0x80, 1);
        }
        d->error = -EIO;
    } else if (packet == 6) {
        d->setup_valid = length == 8;
        if (length == 8)
            for (i = 0; i < 8; ++i)
                d->setup[i] = data[i];
        else {
            stall(d, 0, 1);
            stall(d, 0x80, 1);
        }
        d->out_valid = 0;
    } else if (packet == 2) {
        d->out_length = (uint8_t)length;
        d->out_valid = 1;
        for (i = 0; i < length; ++i)
            d->out[i] = data[i];
    }
    update_reg(d, GINTMSK, 0, RXFLVL);
}
int nk60_usb_poll(struct nk60_usb *d, uint32_t now_ms)
{
    uint32_t flags, ints, pending;
    unsigned loop, ep;
    if (!d || !d->ready)
        return d && d->error ? d->error : -EINVAL;
    d->keyboard.now_ms = now_ms;
    for (loop = 0; loop < 64; ++loop) {
        flags = read_reg(d, GINTSTS);
        if (flags == 0xffffffffu) {
            fail_device(d, -EIO);
            return -EIO;
        }
        flags &= read_reg(d, GINTMSK);
        if (!flags)
            break;
        if (flags & USBRST) {
            write_reg(d, GINTSTS, USBRST);
            d->setup_valid = d->out_valid = 0;
            usb_keyboard_reset(&d->keyboard);
            if (!d->ready)
                return d->error;
            write_reg(d, GRSTCTL, 1u << 4);
            if (wait_bits(d, GRSTCTL, 1u << 4, 0)) {
                fail_device(d, -ETIMEDOUT);
                return d->error;
            }
            open_control(d);
            continue;
        }
        if (flags & ENUMDNE) {
            write_reg(d, GINTSTS, ENUMDNE);
            update_reg(d, DCTL, 0, (1u << 8) | (1u << 10)); /* clear global IN/OUT NAK */
        }
        /* Drain all queued RX entries before handling SETUP-done/XFRC. */
        if (flags & RXFLVL) {
            receive_fifo(d);
            continue;
        }
        if (flags & OEPINT) {
            uint32_t nlp_ints = read_reg(d, OUTINT + 0x40);
            if (nlp_ints) {
                write_reg(d, OUTINT + 0x40, nlp_ints);
                if (nlp_ints & 1u)
                    arm_nlp_out(d);
            }
            ints = read_reg(d, OUTINT);
            if (ints & (1u << 3)) {
                write_reg(d, OUTINT, (1u << 3) | 1u);
                /* A new SETUP invalidates an earlier unobserved IN XFRC. */
                write_reg(d, ININT(0), 0xffffffffu);
                d->tx_busy[0] = 0;
                if (d->setup_valid)
                    usb_keyboard_setup(&d->keyboard, d->setup);
                else {
                    stall(d, 0, 1);
                    stall(d, 0x80, 1);
                }
                d->setup_valid = d->out_valid = 0;
                arm_out(d);
            } else if (ints & 1u) {
                write_reg(d, OUTINT, 1u);
                if (d->out_valid)
                    usb_keyboard_out(&d->keyboard, d->out, d->out_length);
                d->out_valid = 0;
                /* Preserve STALL until the next SETUP; normal status/data OUT
                 * must immediately leave the endpoint ready for SETUP. */
                if (!(read_reg(d, OUTCTL) & STALL))
                    arm_out(d);
            }
            write_reg(d, OUTINT, ints & ~9u);
        }
        if (flags & IEPINT) {
            pending = read_reg(d, DAINT) & read_reg(d, DAINTMSK) & 7u;
            for (ep = 0; ep < 3; ++ep)
                if (pending & (1u << ep)) {
                    ints = read_reg(d, ININT(ep));
                    write_reg(d, ININT(ep), ints);
                    if ((ints & 1u) && d->tx_busy[ep]) {
                        d->tx_busy[ep] = 0;
                        if (ep < 2)
                            usb_keyboard_in_complete(&d->keyboard, (uint8_t)(0x80u | ep));
                    }
                }
        }
        if (flags & USBSUSP) {
            write_reg(d, GINTSTS, USBSUSP);
            usb_keyboard_suspend(&d->keyboard);
        }
        if (flags & WKUPINT) {
            write_reg(d, GINTSTS, WKUPINT);
            usb_keyboard_resume(&d->keyboard);
        }
        if (!d->ready)
            return d->error;
    }
    usb_keyboard_poll(&d->keyboard, now_ms);
    return d->ready ? 0 : d->error;
}
int nk60_usb_nlp_read(struct nk60_usb *d, uint8_t *out, size_t capacity)
{
    unsigned length;
    if (!d || !out || !d->ready || !d->keyboard.configured || !d->nlp_ready)
        return 0;
    length = d->nlp_length;
    if (capacity < length)
        return -EINVAL;
    for (unsigned i = 0; i < length; ++i)
        out[i] = d->nlp_out[i];
    d->nlp_ready = d->nlp_length = 0;
    arm_nlp_out(d);
    return (int)length;
}
int nk60_usb_nlp_write(struct nk60_usb *d, const uint8_t *bytes, size_t length)
{
    if (!d || (!bytes && length))
        return -EINVAL;
    return transmit(d, 0x82, bytes, length);
}
void nk60_usb_disconnect(struct nk60_usb *d)
{
    if (!d)
        return;
    ++d->nlp_generation;
    d->nlp_ready = d->nlp_length = 0;
    update_reg(d, DCTL, 0, 2u);
    write_reg(d, GINTMSK, 0);
    write_reg(d, GAHBCFG, 0);
    d->ready = 0;
}
