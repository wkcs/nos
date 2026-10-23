/* Copyright (C) 2026 NOS contributors. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../servers/usb/keyboard_usb.h"
#include "../boards/nk60_v2/nk60_usb.h"
struct transfer { uint8_t ep, data[64]; size_t length; };
struct host {
    struct usb_keyboard usb;
    struct transfer packet[2048];
    unsigned count, busy[2], stalls[256], cancellations[2];
    uint8_t address, configured, waking;
    int backpressure;
};
static int host_send(void *context, uint8_t ep, const uint8_t *bytes, size_t length)
{
    struct host *h = context;
    unsigned n = ep & 1;
    assert(length <= 64);
    if (h->backpressure || h->busy[n]) return -11;
    assert(h->count < 2048);
    h->packet[h->count].ep = ep; h->packet[h->count].length = length;
    memcpy(h->packet[h->count].data, bytes, length);
    ++h->count; h->busy[n] = 1;
    return 0;
}
static void host_cancel(void *context, uint8_t ep)
{
    struct host *h = context; h->busy[ep & 1] = 0; ++h->cancellations[ep & 1];
}
static void host_stall(void *context, uint8_t ep, int on) { ((struct host *)context)->stalls[ep] = !!on; }
static void host_configure(void *context, int on) { ((struct host *)context)->configured = (uint8_t)on; }
static void host_address(void *context, uint8_t address) { ((struct host *)context)->address = address; }
static void host_wake(void *context, int on) { ((struct host *)context)->waking = (uint8_t)on; }
static void host_init(struct host *h)
{
    struct usb_keyboard_transport t = {h,host_send,host_cancel,host_stall,host_configure,host_address,host_wake};
    memset(h, 0, sizeof(*h)); usb_keyboard_init(&h->usb, &t);
}
static void setup(struct host *h, unsigned type, unsigned request, unsigned value, unsigned index, unsigned length)
{
    uint8_t s[8] = {(uint8_t)type,(uint8_t)request,(uint8_t)value,(uint8_t)(value >> 8),
                    (uint8_t)index,(uint8_t)(index >> 8),(uint8_t)length,(uint8_t)(length >> 8)};
    usb_keyboard_setup(&h->usb, s);
}
static void complete(struct host *h, uint8_t ep)
{
    assert(h->busy[ep & 1]); h->busy[ep & 1] = 0; usb_keyboard_in_complete(&h->usb, ep);
}
static void drain(struct host *h)
{
    unsigned limit = 64;
    while (h->busy[1] && limit--) complete(h, 0x81);
    assert(limit);
}
static void enumerate(struct host *h)
{
    uint8_t previous = h->address;
    setup(h,0,5,7,0,0); assert(h->address == previous); complete(h,0x80); assert(h->address == 7);
    setup(h,0,9,1,0,0); assert(!h->configured); complete(h,0x80); assert(h->configured);
    drain(h);
}
static void validate_report_layout(const uint8_t *bytes, size_t length)
{
    size_t position = 0;
    unsigned size = 0, count = 0, id = 0, inputs[4] = {0}, outputs[4] = {0};
    while (position < length) {
        unsigned prefix = bytes[position++], n = prefix & 3u, value = 0, j;
        if (n == 3) n = 4;
        assert(prefix != 0xfe && position+n <= length);
        for (j = 0; j < n; ++j) value |= (unsigned)bytes[position+j] << (j*8);
        position += n;
        if ((prefix & 0xfc) == 0x74) size = value;
        if ((prefix & 0xfc) == 0x94) count = value;
        if ((prefix & 0xfc) == 0x84) { id = value; assert(id > 0 && id < 4); }
        if ((prefix & 0xfc) == 0x80) inputs[id] += size*count;
        if ((prefix & 0xfc) == 0x90) outputs[id] += size*count;
    }
    assert(inputs[1] == 33*8 && inputs[2] == 16 && inputs[3] == 64);
    assert(outputs[1] == 8 && !outputs[2] && !outputs[3]);
}
static void test_control(void)
{
    struct host h;
    uint8_t descriptor[256], led[2] = {1,3};
    size_t total;
    unsigned first, i;
    host_init(&h);
    setup(&h,0x80,6,0x100,0,8);
    assert(h.packet[0].length == 8 && h.packet[0].data[0] == 18);
    complete(&h,0x80); usb_keyboard_out(&h.usb,0,0);
    setup(&h,0x80,6,0x200,0,255); assert(h.packet[1].length == 34);
    memcpy(descriptor,h.packet[1].data,34);
    assert(descriptor[4] == 1 && descriptor[7] == 0xa0 && descriptor[8] == 250);
    for (i = 0; i < 34; i += descriptor[i]) { assert(descriptor[i] >= 2); assert(i + descriptor[i] <= 34); }
    assert(i == 34 && descriptor[20] == 0x11);
    complete(&h,0x80); usb_keyboard_out(&h.usb,0,0);
    first = h.count; setup(&h,0x81,6,0x2200,0,255);
    total = 0;
    while (h.busy[0]) {
        const struct transfer *p = &h.packet[h.count - 1];
        memcpy(descriptor + total,p->data,p->length); total += p->length;
        complete(&h,0x80);
    }
    assert(h.count > first + 1 && total > 64);
    { size_t expected; const uint8_t *r = usb_keyboard_report_descriptor(&expected);
      assert(total == expected && !memcmp(r,descriptor,total));
      validate_report_layout(descriptor,total); }
    usb_keyboard_out(&h.usb,0,0);
    /* Replacement SETUP cancels old multi-packet descriptor and pending address. */
    setup(&h,0,5,42,0,0); setup(&h,0x80,6,0x100,0,18);
    complete(&h,0x80); assert(!h.address); usb_keyboard_out(&h.usb,0,0);
    setup(&h,0x80,6,0x9900,0,8); assert(h.stalls[0] && h.stalls[0x80]);
    setup(&h,0x80,6,0x100,0,18); assert(!h.stalls[0] && !h.stalls[0x80]);
    complete(&h,0x80); usb_keyboard_out(&h.usb,0,0);
    enumerate(&h);
    setup(&h,0x21,9,0x201,0,2); usb_keyboard_out(&h.usb,led,2);
    assert(!usb_keyboard_leds(&h.usb)); complete(&h,0x80); assert(usb_keyboard_leds(&h.usb) == 3);
    setup(&h,0xa1,1,0x201,0,2); assert(h.packet[h.count-1].data[1] == 3);
    complete(&h,0x80); usb_keyboard_out(&h.usb,0,0);
    setup(&h,0x21,9,0x201,0,2); usb_keyboard_out(&h.usb,led,1); assert(h.stalls[0]);
    setup(&h,0x21,0xb,0,0,0); complete(&h,0x80); drain(&h); assert(!h.usb.protocol);
    led[0] = 2; setup(&h,0x21,9,0x200,0,1); usb_keyboard_out(&h.usb,led,1); complete(&h,0x80);
    assert(usb_keyboard_leds(&h.usb) == 2);
    setup(&h,0xa1,3,0,0,1); assert(h.packet[h.count-1].data[0] == 0);
    complete(&h,0x80); usb_keyboard_out(&h.usb,0,0);
    setup(&h,0x21,0xb,2,0,0); assert(h.stalls[0]);
    setup(&h,0x21,9,0x201,1,2); assert(h.stalls[0]);
    usb_keyboard_reset(&h.usb); assert(!h.address && !h.configured && h.usb.protocol == 1);
}
static void test_reports(void)
{
    struct host h;
    uint8_t boot[8] = {0}, nkro[33] = {0}, consumer[2] = {0};
    unsigned start, i;
    host_init(&h); enumerate(&h); start = h.count;
    boot[2] = 4; nkro[1] = 16; consumer[0] = 0xe9;
    assert(!usb_keyboard_submit(&h.usb,boot,nkro,consumer));
    assert(h.packet[start].length == 34 && h.packet[start].data[0] == 1 && h.packet[start].data[2] == 16);
    memset(boot,0,8); memset(nkro,0,33); memset(consumer,0,2);
    assert(!usb_keyboard_submit(&h.usb,boot,nkro,consumer));
    drain(&h); assert(h.count == start + 4);
    assert(h.packet[start+1].data[0] == 2 && h.packet[start+1].data[1] == 0xe9);
    assert(h.packet[start+2].data[0] == 1 && h.packet[start+2].data[2] == 0);
    assert(h.packet[start+3].data[0] == 2 && h.packet[start+3].data[1] == 0);
    /* A stopped host cannot overwrite queued release packets. */
    h.backpressure = 1;
    for (i = 0; i < USB_KBD_QUEUE; ++i) {
        boot[2] = (i & 1) ? 0 : 4; nkro[1] = (i & 1) ? 0 : 16;
        assert(!usb_keyboard_submit(&h.usb,boot,nkro,consumer));
    }
    boot[2] = 4; nkro[1] = 16;
    assert(usb_keyboard_submit(&h.usb,boot,nkro,consumer) == -11);
    h.backpressure = 0; usb_keyboard_poll(&h.usb,10); start = h.count - 1; drain(&h);
    assert(h.count == start + USB_KBD_QUEUE);
    for (i = 0; i < USB_KBD_QUEUE; ++i) assert(h.packet[start+i].data[2] == ((i & 1) ? 0 : 16));
    assert(!usb_keyboard_submit(&h.usb,boot,nkro,consumer)); drain(&h);
    start = h.count; assert(!usb_keyboard_set_nkro(&h.usb,0)); drain(&h);
    assert(h.packet[start].data[0] == 1 && h.packet[start].data[2] == 0);
    assert(h.packet[start+1].data[0] == 3 && h.packet[start+1].data[3] == 4);
    setup(&h,0x21,0xb,0,0,0); complete(&h,0x80); start = h.count - 1; drain(&h);
    assert(h.packet[start].length == 8 && h.packet[start].data[2] == 4);
    consumer[0] = 0xe9; start = h.count; assert(!usb_keyboard_submit(&h.usb,boot,nkro,consumer));
    assert(h.count == start);
    setup(&h,2,3,0,0x81,0); complete(&h,0x80); assert(h.stalls[0x81]);
    boot[2] = 0; assert(!usb_keyboard_submit(&h.usb,boot,nkro,consumer)); assert(!h.busy[1]);
    setup(&h,2,1,0,0x81,0); complete(&h,0x80); assert(!h.stalls[0x81]); drain(&h);
}
static void test_idle_and_suspend(void)
{
    struct host h;
    unsigned start;
    host_init(&h); enumerate(&h);
    setup(&h,0x21,0xa,0x0202,0,0); complete(&h,0x80);
    setup(&h,0xa1,2,2,0,1); assert(h.packet[h.count-1].data[0] == 2);
    complete(&h,0x80); usb_keyboard_out(&h.usb,0,0);
    start = h.count; usb_keyboard_poll(&h.usb,7); assert(h.count == start);
    usb_keyboard_poll(&h.usb,8); assert(h.count == start+1 && h.packet[start].data[0] == 2); drain(&h);
    setup(&h,0x21,0xa,0x0100,0,0); complete(&h,0x80);
    start = h.count; usb_keyboard_poll(&h.usb,12); drain(&h); assert(h.count == start+2);
    usb_keyboard_suspend(&h.usb); assert(usb_keyboard_remote_wake(&h.usb) == -13);
    setup(&h,0,3,1,0,0); complete(&h,0x80);
    assert(usb_keyboard_remote_wake(&h.usb) == -11);
    start = h.count; usb_keyboard_poll(&h.usb,17); assert(h.count == start);
    assert(!usb_keyboard_remote_wake(&h.usb) && h.waking);
    usb_keyboard_poll(&h.usb,18); assert(h.waking);
    usb_keyboard_poll(&h.usb,19); assert(!h.waking);
    usb_keyboard_resume(&h.usb); assert(!h.usb.suspended);
    setup(&h,0,9,0,0,0); complete(&h,0x80); assert(!h.configured && !h.usb.remote_enabled);
    /* Wrap-safe suspend and idle timing. */
    enumerate(&h); usb_keyboard_poll(&h.usb,0xfffffffeu); usb_keyboard_suspend(&h.usb);
    setup(&h,0,3,1,0,0); complete(&h,0x80); usb_keyboard_poll(&h.usb,3);
    assert(!usb_keyboard_remote_wake(&h.usb)); usb_keyboard_poll(&h.usb,5); assert(!h.waking);
}

/* Register-accurate enough to enforce DWC2 W1C, RX status/FIFO ownership,
 * endpoint enables, transfer completion, and per-endpoint transmit FIFOs. */
struct dwc2 {
    uint32_t reg[0x2100/4], gpio[16], rcc[32], us;
    uint32_t rx_status[8], rx_words[8][32];
    unsigned rx_head, rx_count, rx_word, current_rx, reads, stuck_reset;
    uint32_t tx[2][256]; unsigned tx_count[2];
};
static uint32_t dw_read(void *context, uintptr_t address)
{
    struct dwc2 *m = context;
    unsigned off;
    if (address >= NK60_GPIO_A && address < NK60_GPIO_A+64) return m->gpio[(address-NK60_GPIO_A)/4];
    if (address >= NK60_RCC && address < NK60_RCC+128) return m->rcc[(address-NK60_RCC)/4];
    assert(address >= NK60_USB_BASE && address < NK60_USB_BASE+0x2100 && !(address & 3));
    off = (unsigned)(address-NK60_USB_BASE);
    if (off == 0x010) return m->stuck_reset ? 1u : 1u << 31;
    if (off == 0x918 || off == 0x938) return 64;
    if (off == 0x014) {
        uint32_t value = m->reg[off/4];
        if (m->rx_count) value |= 1u << 4;
        if (m->reg[0xb08/4] & m->reg[0x814/4]) value |= 1u << 19;
        if ((m->reg[0x908/4] | m->reg[0x928/4]) & m->reg[0x810/4]) value |= 1u << 18;
        return value;
    }
    if (off == 0x818) return (m->reg[0x908/4] ? 1u : 0) | (m->reg[0x928/4] ? 2u : 0);
    if (off == 0x020) {
        uint32_t result;
        assert(m->rx_count); m->current_rx = m->rx_head; m->rx_word = 0;
        result = m->rx_status[m->rx_head]; m->rx_head = (m->rx_head+1)%8; --m->rx_count;
        return result;
    }
    if (off == 0x1000) { assert(m->rx_word < 32); ++m->reads; return m->rx_words[m->current_rx][m->rx_word++]; }
    return m->reg[off/4];
}
static void dw_write(void *context, uintptr_t address, uint32_t value)
{
    struct dwc2 *m = context;
    unsigned off, ep;
    if (address >= NK60_GPIO_A && address < NK60_GPIO_A+64) { m->gpio[(address-NK60_GPIO_A)/4] = value; return; }
    if (address >= NK60_RCC && address < NK60_RCC+128) { m->rcc[(address-NK60_RCC)/4] = value; return; }
    assert(address >= NK60_USB_BASE && address < NK60_USB_BASE+0x2100 && !(address & 3));
    off = (unsigned)(address-NK60_USB_BASE);
    if (off == 0x014 || off == 0x908 || off == 0x928 || off == 0xb08) { m->reg[off/4] &= ~value; return; }
    if (off == 0x010 && (value & (1u << 4))) m->rx_count = 0;
    if (off == 0x1000 || off == 0x2000) {
        ep = off == 0x2000; assert(m->tx_count[ep] < 256); m->tx[ep][m->tx_count[ep]++] = value; return;
    }
    if (off == 0x900 || off == 0x920) {
        if (value & (1u << 30)) value &= ~(1u << 31);
    }
    m->reg[off/4] = value;
}
static uint32_t dw_time(void *context) { return ((struct dwc2 *)context)->us++; }
static void dw_delay(void *context, uint32_t delay) { ((struct dwc2 *)context)->us += delay; }
static void dw_rx(struct dwc2 *m, unsigned packet, const uint8_t *bytes, unsigned length)
{
    unsigned tail = (m->rx_head+m->rx_count)%8, i;
    assert(m->rx_count < 8 && length <= 128);
    m->rx_status[tail] = (packet << 17) | (length << 4);
    memset(m->rx_words[tail],0,sizeof(m->rx_words[tail]));
    for (i = 0; i < length; ++i) m->rx_words[tail][i/4] |= (uint32_t)bytes[i] << ((i%4)*8);
    ++m->rx_count;
}
static void dw_setup(struct dwc2 *m, struct nk60_usb *d, unsigned type, unsigned request,
                      unsigned value, unsigned length)
{
    uint8_t s[8] = {(uint8_t)type,(uint8_t)request,(uint8_t)value,(uint8_t)(value >> 8),0,0,
                   (uint8_t)length,(uint8_t)(length >> 8)};
    dw_rx(m,6,s,8); m->reg[0xb08/4] |= 8u; assert(!nk60_usb_poll(d,1));
}
static void dw_complete(struct dwc2 *m, struct nk60_usb *d, unsigned ep)
{
    m->reg[(0x908+ep*32)/4] |= 1u;
    m->reg[(0x900+ep*32)/4] &= ~(1u << 31);
    assert(!nk60_usb_poll(d,2));
}
static void test_dwc2(void)
{
    struct dwc2 m;
    struct nk60_usb d;
    struct nk60_io io = {&m,dw_read,dw_write,0,0,dw_time,dw_delay};
    uint8_t led[2] = {1,5}, oversized[65] = {0};
    unsigned reads;
    memset(&m,0,sizeof(m)); assert(!nk60_usb_init(&d,&io));
    assert(m.rcc[0x30/4] & 1u); assert(m.rcc[0x34/4] & 128u);
    assert(((m.gpio[0x24/4] >> 12) & 255u) == 0xaau);
    assert(m.reg[0x024/4] == 128 && m.reg[0x104/4] == ((64u << 16)|192));
    m.reg[0x014/4] |= 1u << 12; assert(!nk60_usb_poll(&d,0));
    assert(d.keyboard.address == 0 && d.ready);
    dw_setup(&m,&d,0x80,6,0x100,18);
    assert(m.tx_count[0] == 5 && (m.tx[0][0] & 0xffff) == 0x0112);
    dw_complete(&m,&d,0);
    dw_rx(&m,2,0,0); m.reg[0xb08/4] |= 1u; assert(!nk60_usb_poll(&d,3));
    dw_setup(&m,&d,0,5,9,0); assert(!(m.reg[0x800/4] & (127u << 4)));
    dw_complete(&m,&d,0); assert(((m.reg[0x800/4] >> 4) & 127) == 9);
    dw_setup(&m,&d,0,9,1,0); dw_complete(&m,&d,0);
    assert(m.reg[0x81c/4] & 2u); assert(d.tx_busy[1]);
    dw_complete(&m,&d,1); dw_complete(&m,&d,1);
    assert(!d.tx_busy[1] && m.tx_count[1] == 10);
    dw_setup(&m,&d,0x21,9,0x201,2);
    dw_rx(&m,2,led,2); m.reg[0xb08/4] |= 1u; assert(!nk60_usb_poll(&d,4));
    assert(d.keyboard.leds == 0); dw_complete(&m,&d,0); assert(d.keyboard.leds == 5);
    dw_setup(&m,&d,0x80,6,0x9900,5);
    assert(m.reg[0xb00/4] & (1u << 21));
    dw_setup(&m,&d,0x80,6,0x100,8); assert(!(m.reg[0xb00/4] & (1u << 21)));
    /* Old completion racing with replacement SETUP cannot acknowledge address. */
    dw_setup(&m,&d,0,5,10,0); /* invalid while configured => stall */
    dw_setup(&m,&d,0,9,0,0); dw_complete(&m,&d,0);
    dw_setup(&m,&d,0,5,10,0);
    m.reg[0x908/4] |= 1u;
    dw_setup(&m,&d,0x80,6,0x100,8);
    assert(d.keyboard.address == 9 && d.tx_busy[0]);
    reads = m.reads; dw_rx(&m,2,oversized,65); assert(!nk60_usb_poll(&d,5));
    assert(m.reads == reads+17 && d.error == -5);
    assert(m.reg[0xb00/4] & (1u << 21));
    dw_rx(&m,6,oversized,8); /* bus reset must discard pre-reset setup bytes */
    m.reg[0x014/4] |= 1u << 12; assert(!nk60_usb_poll(&d,6));
    assert(!d.keyboard.configured && !d.keyboard.address && !m.rx_count);
    nk60_usb_disconnect(&d); assert(!d.ready && (m.reg[0x804/4] & 2u));
    memset(&m,0,sizeof(m)); m.stuck_reset = 1;
    assert(nk60_usb_init(&d,&io) == -110 && !d.ready && m.reg[0x018/4] == 0);
}
static void test_setup_fuzz(void)
{
    struct host h;
    uint32_t seed = 0x760;
    unsigned i,j;
    host_init(&h); enumerate(&h);
    for (i = 0; i < 10000; ++i) {
        uint8_t s[8], out[64];
        for (j = 0; j < 8; ++j) { seed = seed*1664525u+1013904223u; s[j] = (uint8_t)(seed >> 24); }
        memset(out,0,sizeof(out));
        usb_keyboard_setup(&h.usb,s);
        if (h.busy[0]) complete(&h,0x80);
        usb_keyboard_out(&h.usb,out,(i%65));
        assert(h.usb.count <= USB_KBD_QUEUE && h.usb.control_length <= sizeof(h.usb.control));
        if (h.count > 1900) h.count = 0;
    }
}
int main(void)
{
    test_control(); puts("PASS usb_ep0_descriptor_layout_setup_cancel_address_led_stall");
    test_reports(); puts("PASS usb_boot_nkro_media_queue_backpressure_release_halt");
    test_idle_and_suspend(); puts("PASS usb_per_report_idle_suspend_host_optin_wake_wrap");
    test_dwc2(); puts("PASS usb_dwc2_gpio_clock_fifo_w1c_setup_out_completion_timeout");
    test_setup_fuzz(); puts("PASS usb_10000_malformed_setup_and_out_sequences");
    puts("USB keyboard: EP0, descriptors, HID protocol/LED/idle, queue, wake and DWC2 register model passed");
    return 0;
}
