/* Copyright (C) 2026 NOS contributors. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../servers/keyboard/service.h"
#include "nlp/hash.h"

struct model {
    uint32_t now_us;
    uint64_t pressed;
    struct nk60_usb *usb;
    struct nk60_hw *hardware;
    uint8_t flash[4u * KB_CONFIG_SLOT_BYTES];
    uint8_t nlp_rx[4096], nlp_tx[4096];
    size_t nlp_rx_size, nlp_rx_at, nlp_tx_size;
    char output[65536];
    size_t output_bytes;
    const char *input;
    uint8_t packets[1024][34], packet_bytes[1024];
    unsigned packet_count, polls, erases, writes, dma_count;
    unsigned progress_scans, progress_polls;
    unsigned powered, refuse_in, refuse_uart, erase_fails;
    unsigned change_during_save;
    unsigned usb_fault;
    unsigned wake_count;
};
static struct model m;
static int nlp_model;
static struct nk60_service service;
const uint8_t nk60_canvas_to_led[NK60_CANVAS_WIDTH * NK60_CANVAS_HEIGHT] = {0};

static uint32_t io_read32(void *context, uintptr_t address)
{
    (void)context;
    if (nlp_model && address >= 0x1fff7a10u && address <= 0x1fff7a18u)
        return 0xabcd1234u + (uint32_t)(address - 0x1fff7a10u);
    return 0;
}
static void io_write32(void *context, uintptr_t address, uint32_t value)
{
    (void)context;
    (void)address;
    (void)value;
}
static uint8_t io_read8(void *context, uintptr_t address)
{
    (void)context;
    (void)address;
    return 0;
}
static void io_write8(void *context, uintptr_t address, uint8_t value)
{
    (void)context;
    (void)address;
    (void)value;
}
static uint32_t io_time(void *context)
{
    return ((struct model *)context)->now_us;
}
static void io_delay(void *context, uint32_t us)
{
    ((struct model *)context)->now_us += us;
}
static int uart_read(void *context)
{
    struct model *model = context;
    if (!model->input || !*model->input)
        return -11;
    return (uint8_t)*model->input++;
}
static int uart_write(void *context, const char *bytes, size_t length)
{
    struct model *model = context;
    assert(length == 1);
    if (model->refuse_uart)
        return -11;
    assert(model->output_bytes + length < sizeof(model->output));
    memcpy(model->output + model->output_bytes, bytes, length);
    model->output_bytes += length;
    model->output[model->output_bytes] = 0;
    return 0;
}
static int dma(void *context, uintptr_t destination, const uint8_t *bytes, size_t length,
               uint32_t timeout)
{
    struct model *model = context;
    (void)bytes;
    assert(destination == NK60_SPI2 + NK60_SPI_DR && length == NK60_RGB_WAVE_BYTES && timeout);
    ++model->dma_count;
    return 0;
}

/* Physical drivers are modeled here. Their register behavior is verified in
 * host_nk60_hardware; this test links actual service+engine+NOR journal+USB core. */
int nk60_hw_bind(struct nk60_hw *hw, const struct nk60_io *io)
{
    memset(hw, 0, sizeof(*hw));
    hw->io = *io;
    m.hardware = hw;
    return 0;
}
void nk60_hw_set_progress(struct nk60_hw *hw, void (*callback)(void *), void *context)
{
    hw->progress = callback;
    hw->progress_context = context;
}
int nk60_matrix_init(struct nk60_hw *hw)
{
    hw->matrix_ready = 1;
    return 0;
}
int nk60_matrix_scan(struct nk60_hw *hw, uint64_t *pressed)
{
    (void)hw;
    *pressed = m.pressed;
    m.now_us += 50;
    if (service.saving)
        ++m.progress_scans;
    return 0;
}
int nk60_flash_init(struct nk60_hw *hw)
{
    hw->flash_ready = 1;
    hw->flash_bytes = sizeof(m.flash);
    return 0;
}
int nk60_flash_read(struct nk60_hw *hw, uint32_t offset, void *bytes, size_t length)
{
    (void)hw;
    assert(offset <= sizeof(m.flash) && length <= sizeof(m.flash) - offset);
    memcpy(bytes, m.flash + offset, length);
    return 0;
}
int nk60_flash_program(struct nk60_hw *hw, uint32_t offset, const void *bytes, size_t length)
{
    const uint8_t *source = bytes;
    (void)hw;
    assert(offset <= sizeof(m.flash) && length <= sizeof(m.flash) - offset);
    for (size_t i = 0; i < length; ++i) {
        assert((m.flash[offset + i] & source[i]) == source[i]);
        m.flash[offset + i] &= source[i];
    }
    ++m.writes;
    return 0;
}
int nk60_flash_erase_sector(struct nk60_hw *hw, uint32_t offset)
{
    assert(!(offset % KB_CONFIG_SLOT_BYTES) && offset < sizeof(m.flash));
    ++m.erases;
    for (unsigned i = 0; i < 40; ++i) {
        m.now_us += 5000;
        if (m.change_during_save)
            m.pressed = i >= 5 && i < 20 ? UINT64_C(1) << 29 : 0;
        if (hw->progress)
            hw->progress(hw->progress_context);
    }
    if (m.erase_fails)
        return -5;
    memset(m.flash + offset, 0xff, KB_CONFIG_SLOT_BYTES);
    return 0;
}
int nk60_rgb_init(struct nk60_hw *hw)
{
    hw->rgb_ready = 1;
    return 0;
}
int nk60_rgb_set_power(struct nk60_hw *hw, int enabled)
{
    hw->rgb_powered = (uint8_t) !!enabled;
    m.powered = !!enabled;
    return 0;
}
int nk60_rgb_encode(const struct nk60_rgb *colors, size_t count, uint8_t *wave, size_t length)
{
    unsigned sum = 0;
    assert(count == NK60_LED_COUNT && length == NK60_RGB_WAVE_BYTES);
    for (unsigned i = 0; i < count; ++i)
        sum += colors[i].r + colors[i].g + colors[i].b;
    assert(sum <= 3825u); /* 300mA modeled LED current ceiling. */
    memset(wave, 0xc0, length);
    return 0;
}
int nk60_rgb_write(struct nk60_hw *hw, const uint8_t *wave, size_t length, nk60_dma_tx_fn callback,
                   void *context)
{
    (void)hw;
    return callback(context, NK60_SPI2 + NK60_SPI_DR, wave, length, 10000);
}
static int usb_send(void *context, uint8_t endpoint, const uint8_t *bytes, size_t length)
{
    struct model *model = context;
    if (endpoint != 0x81)
        return 0;
    if (model->refuse_in)
        return -11;
    assert(model->packet_count < 1024 && length <= 34);
    memcpy(model->packets[model->packet_count], bytes, length);
    model->packet_bytes[model->packet_count++] = (uint8_t)length;
    return 0;
}
static void usb_cancel(void *context, uint8_t endpoint)
{
    (void)context;
    (void)endpoint;
}
static void usb_stall(void *context, uint8_t endpoint, int enabled)
{
    (void)context;
    (void)endpoint;
    (void)enabled;
}
static void usb_configure(void *context, int enabled)
{
    (void)context;
    (void)enabled;
}
static void usb_address(void *context, uint8_t address)
{
    (void)context;
    (void)address;
}
static void usb_wake(void *context, int enabled)
{
    if (enabled)
        ++((struct model *)context)->wake_count;
}
int nk60_usb_init(struct nk60_usb *usb, const struct nk60_io *io)
{
    struct usb_keyboard_transport transport = {
        &m, usb_send, usb_cancel, usb_stall, usb_configure, usb_address, usb_wake};
    memset(usb, 0, sizeof(*usb));
    usb->io = *io;
    m.usb = usb;
    usb_keyboard_init(&usb->keyboard, &transport);
    usb->ready = 1;
    return 0;
}
int nk60_usb_nlp_read(struct nk60_usb *usb, uint8_t *bytes, size_t size)
{
    (void)usb;
    size_t count = m.nlp_rx_size - m.nlp_rx_at;
    if (count > size)
        count = size;
    if (count > 64)
        count = 64;
    memcpy(bytes, m.nlp_rx + m.nlp_rx_at, count);
    m.nlp_rx_at += count;
    return (int)count;
}
int nk60_usb_nlp_write(struct nk60_usb *usb, const uint8_t *bytes, size_t size)
{
    (void)usb;
    if (sizeof(m.nlp_tx) - m.nlp_tx_size < size)
        return -11;
    memcpy(m.nlp_tx + m.nlp_tx_size, bytes, size);
    m.nlp_tx_size += size;
    return 0;
}
int nk60_usb_poll(struct nk60_usb *usb, uint32_t ms)
{
    ++m.polls;
    if (m.usb_fault)
        return -5;
    if (service.saving)
        ++m.progress_polls;
    if (usb->keyboard.in_busy && !m.refuse_in)
        usb_keyboard_in_complete(&usb->keyboard, 0x81);
    usb_keyboard_poll(&usb->keyboard, ms);
    return 0;
}

static void setup(void)
{
    const struct nk60_io io = {&m, io_read32, io_write32, io_read8, io_write8, io_time, io_delay};
    const struct nk60_service_hooks hooks = {&m, uart_read, uart_write, dma};
    memset(&m, 0, sizeof(m));
    memset(m.flash, 0xff, sizeof(m.flash));
    assert(!nk60_service_init(&service, &io, &hooks));
    service.usb.keyboard.configured = 1;
    nk60_service_step(&service);
}
static void steps(unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        m.now_us += 4000;
        nk60_service_step(&service);
    }
}
static void drain_output(void)
{
    for (unsigned i = 0; i < 2000 && service.tx_count; ++i)
        nk60_service_step(&service);
    assert(!service.tx_count);
}
static int packet_key(unsigned at, unsigned usage)
{
    return m.packet_bytes[at] == 34 && m.packets[at][0] == 1 &&
           (m.packets[at][2 + usage / 8] & (1u << (usage % 8))) != 0;
}
static void test_reports(void)
{
    setup();
    steps(3);
    m.pressed = UINT64_C(1) << 29;
    steps(4);
    m.pressed = 0;
    steps(4);
    unsigned press = 0, release = 0;
    for (unsigned i = 0; i < m.packet_count; ++i) {
        if (packet_key(i, KB_A))
            press = i + 1;
        if (press && i + 1 > press && m.packet_bytes[i] == 34 && !packet_key(i, KB_A))
            release = i + 1;
    }
    assert(press && release > press);
    puts("PASS nk60_service_real_engine_usb_press_release");
}
static void test_backpressure(void)
{
    setup();
    steps(3);
    m.refuse_in = 1;
    uint8_t boot[8] = {0}, nkro[33] = {0}, consumer[2] = {0};
    for (unsigned i = 0; i < USB_KBD_QUEUE; ++i) {
        nkro[2] = (uint8_t)(i + 1);
        assert(!usb_keyboard_submit(&service.usb.keyboard, boot, nkro, consumer));
    }
    m.pressed = UINT64_C(1) << 29;
    steps(4);
    assert(kb_report_peek(&service.keyboard));
    m.pressed = 0;
    steps(4);
    assert(kb_report_peek(&service.keyboard));
    m.refuse_in = 0;
    steps(30);
    assert(!kb_report_peek(&service.keyboard));
    unsigned press = 0, release = 0;
    for (unsigned i = 0; i < m.packet_count; ++i) {
        if (packet_key(i, KB_A))
            press = i + 1;
        if (press && i + 1 > press && m.packet_bytes[i] == 34 && !packet_key(i, KB_A))
            release = i + 1;
    }
    assert(press && release > press);
    puts("PASS nk60_service_usb_backpressure_retains_release");
}
static void unlock(void)
{
    m.pressed = (UINT64_C(1) << 0) | (UINT64_C(1) << 58) | (UINT64_C(1) << 60);
    steps(503);
    assert(service.unlocked);
    m.pressed = 0;
    steps(4);
}
static void test_uart_security(void)
{
    setup();
    steps(3);
    nk60_service_command(&service, "bind 0 29 2 0 5");
    assert(service.keyboard.config.map[0][29].code == KB_A && !service.dirty);
    unlock();
    nk60_service_command(&service, "bind 0 29 2 0 5");
    assert(service.keyboard.config.map[0][29].code == KB_B && service.dirty);
    nk60_service_command(&service, "bind 0 61 2 0 5");
    nk60_service_command(&service, "bind 4294967296 0 2 0 5");
    assert(service.keyboard.config.map[0][29].code == KB_B);
    m.pressed = (UINT64_C(1) << 0) | (UINT64_C(1) << 58) | (UINT64_C(1) << 60);
    steps(503);
    nk60_service_command(&service, "lock");
    steps(4);
    assert(!service.unlocked); /* Holding a consumed chord never rearms. */
    m.pressed = 0;
    steps(4);
    unlock();
    steps(7501);
    assert(!service.unlocked);
    m.refuse_uart = 1;
    unsigned before = service.tx_count;
    steps(1);
    assert(service.tx_count >= before);
    for (unsigned i = 0; i < 10; ++i)
        nk60_service_command(&service, "status");
    assert(service.tx_count <= NK60_SERVICE_TX_BYTES && service.tx_dropped);
    m.refuse_uart = 0;
    drain_output();
    char input[128];
    memset(input, 'x', 110);
    memcpy(input + 110, "\r\nstatus\r\n", 11);
    m.input = input;
    steps(10);
    drain_output();
    assert(!service.line_length && !service.line_overflow);
    assert(strstr(m.output, "ERR line too long"));
    assert(strstr(m.output, "ERR locked") && strstr(m.output, "OK changed"));
    puts("PASS nk60_service_physical_unlock_parser_and_nonblocking_uart");
}
static void test_persist_progress(void)
{
    setup();
    steps(3);
    unlock();
    nk60_service_command(&service, "bind 0 29 2 0 5");
    steps(4);
    assert(!m.erases);
    nk60_service_command(&service, "save");
    m.change_during_save = 1;
    steps(1);
    assert(m.erases == 1 && service.generation == 1 && !service.dirty);
    assert(m.progress_scans >= 35 && m.progress_polls >= 70);
    struct kb_config loaded;
    uint32_t generation = 0;
    assert(!kb_config_load(&service.store, service.store_base, &loaded, &generation));
    assert(generation == 1 && loaded.map[0][29].code == KB_B);
    unsigned press = 0, release = 0;
    for (unsigned i = 0; i < m.packet_count; ++i) {
        if (packet_key(i, KB_B))
            press = i + 1;
        if (press && i + 1 > press && m.packet_bytes[i] == 34 && !packet_key(i, KB_B))
            release = i + 1;
    }
    assert(press && release > press); /* Key events survive the erase interval. */
    nk60_service_command(&service, "bind 0 29 2 0 6");
    m.erase_fails = 1;
    nk60_service_command(&service, "save");
    steps(4);
    assert(service.save_errors == 1 && service.dirty && service.generation == 1);
    assert(!kb_config_load(&service.store, service.store_base, &loaded, &generation));
    assert(generation == 1 && loaded.map[0][29].code == KB_B);
    puts("PASS nk60_service_flash_progress_keeps_usb_and_scan_alive");
}
static void test_macro_configuration(void)
{
    setup();
    steps(3);
    unlock();
    nk60_service_command(&service, "macro 0 4 5");
    assert(service.keyboard.config.macro_length[0] == 2);
    nk60_service_command(&service, "macro 0 4 5 6 7 8 9 10 11 12");
    assert(service.keyboard.config.macro_length[0] == 2);
    nk60_service_command(&service, "bind 0 29 10 0 0");
    steps(4);
    m.pressed = UINT64_C(1) << 29;
    steps(40);
    m.pressed = 0;
    steps(4);
    unsigned a = 0, b = 0;
    for (unsigned i = 0; i < m.packet_count; ++i) {
        if (packet_key(i, KB_A))
            a = i + 1;
        if (packet_key(i, KB_B))
            b = i + 1;
    }
    assert(a && b > a);
    nk60_service_command(&service, "macro 0");
    assert(!service.keyboard.config.macro_length[0]);
    puts("PASS nk60_service_uart_macro_configuration_executes");
}
static void test_suspend_power_and_clock_wrap(void)
{
    setup();
    steps(3);
    unlock();
    nk60_service_command(&service, "rgb 1 255");
    steps(30);
    assert(m.powered && m.dma_count);
    usb_keyboard_suspend(&service.usb.keyboard);
    nk60_service_step(&service);
    assert(!m.powered && service.keyboard.blocked);
    service.usb.keyboard.remote_enabled = 1;
    m.pressed = UINT64_C(1) << 29;
    steps(30);
    assert(m.wake_count == 1);
    m.pressed = 0;
    steps(4);
    m.pressed = UINT64_C(1) << 29;
    steps(4);
    assert(m.wake_count == 2);
    m.pressed = 0;
    steps(4);
    usb_keyboard_resume(&service.usb.keyboard);
    steps(30);
    assert(m.powered);
    service.usb.keyboard.configured = 0;
    nk60_service_step(&service);
    assert(!m.powered);
    m.now_us = UINT32_MAX - 500u;
    service.last_us = m.now_us;
    service.next_scan_us = m.now_us;
    uint32_t before = service.now_ms;
    m.now_us += 1500u;
    nk60_service_step(&service);
    assert(service.now_ms - before >= 1 && service.now_ms - before <= 2);
    service.usb.keyboard.configured = 1;
    m.usb_fault = 1;
    nk60_service_step(&service);
    assert(!service.usb_ready && service.usb_errors == 1 && !m.powered);
    puts("PASS nk60_service_suspend_rgb_budget_and_microsecond_wrap");
}
/* Real service + NLP + canonical keyboard backend, with USB packet I/O modeled. */
struct link_client {
    struct nlp_pool pool;
    struct nlp_session session;
    uint8_t response[128];
    uint16_t response_size;
    uint32_t request;
    int ready;
};
static struct link_client link_clients[2];
static struct nlp_stream host_stream;
static uint8_t control_bytes[128];
static unsigned control_size, control_opcode;
static int host_to_keyboard(void *context, const uint8_t *bytes, size_t size)
{
    (void)context;
    if (m.nlp_rx_at == m.nlp_rx_size)
        m.nlp_rx_at = m.nlp_rx_size = 0;
    if (sizeof(m.nlp_rx) - m.nlp_rx_size < size + 2)
        return -NLP_BUSY;
    nlp_put16(m.nlp_rx + m.nlp_rx_size, (uint16_t)size);
    memcpy(m.nlp_rx + m.nlp_rx_size + 2, bytes, size);
    m.nlp_rx_size += size + 2;
    return 0;
}
static int client_message(void *context, struct nlp_session *session, const struct nlp_frame *f)
{
    struct link_client *c = context;
    (void)session;
    assert(f->h.kind == NLP_RESPONSE && f->length <= 128);
    memcpy(c->response, f->payload, f->length);
    c->response_size = f->length;
    c->ready = 1;
    return 0;
}
static int keyboard_to_host(void *unused, const uint8_t *raw, size_t size)
{
    struct nlp_frame f;
    (void)unused;
    assert(!nlp_frame_decode(raw, size, &f));
    if (!f.h.session_id) {
        assert(f.length <= 128);
        memcpy(control_bytes, f.payload, f.length);
        control_size = f.length;
        control_opcode = f.h.opcode;
        return 0;
    }
    for (unsigned i = 0; i < 2; ++i)
        if (link_clients[i].session.active && link_clients[i].session.id == f.h.session_id)
            return nlp_session_receive(&link_clients[i].session, raw, size, service.now_ms);
    return 0;
}
static void link_drive(void)
{
    for (unsigned i = 0; i < 2; ++i)
        nlp_session_poll(&link_clients[i].session, service.now_ms);
    m.now_us += 4000;
    nk60_service_step(&service);
    if (m.nlp_tx_size) {
        size_t consumed;
        assert(!nlp_stream_feed(&host_stream, m.nlp_tx, m.nlp_tx_size, &consumed, service.now_ms,
                                keyboard_to_host, NULL));
        assert(consumed == m.nlp_tx_size);
        m.nlp_tx_size = 0;
    }
}
static void client_start(struct link_client *c, uint32_t id, uint16_t cap, uint16_t node)
{
    memset(c, 0, sizeof(*c));
    nlp_session_init(&c->session, &c->pool);
    c->session.id = id;
    c->session.epoch = service.link.endpoint.config.epoch;
    c->session.local_node = node;
    c->session.remote_node = 1;
    c->session.local_cap = 0;
    c->session.remote_cap = cap;
    c->session.send = host_to_keyboard;
    c->session.message = client_message;
    c->session.context = c;
    assert(!nlp_session_start(&c->session, service.now_ms));
}
static void send_control(unsigned opcode, const uint8_t *body, size_t size)
{
    struct nlp_header h = {.kind = 0,
                           .priority = 0,
                           .hop_limit = 1,
                           .src_node = 65534,
                           .dst_node = opcode == 1 ? 65535 : 1,
                           .opcode = (uint16_t)opcode};
    uint8_t payload[128], raw[NLP_PDU_MAX];
    size_t written;
    h.fabric_epoch = opcode == 1 ? 0 : service.link.endpoint.config.epoch;
    nlp_put32(payload, opcode);
    nlp_put16(payload + 4, 0);
    nlp_put16(payload + 6, 0);
    memcpy(payload + 8, body, size);
    assert(!nlp_frame_encode(&h, payload, size + 8, raw, sizeof(raw), &written));
    assert(!host_to_keyboard(NULL, raw, written));
    control_opcode = 0;
    for (unsigned i = 0; i < 500 && !control_opcode; ++i)
        link_drive();
    assert(control_opcode == (opcode == 1 ? 2 : 4) && !nlp_get16(control_bytes + 4));
}
static unsigned link_rpc(struct link_client *c, unsigned op, const uint8_t *body, size_t size)
{
    uint8_t payload[128];
    assert(size <= 120);
    nlp_put32(payload, ++c->request);
    memset(payload + 4, 0, 4);
    if (size)
        memcpy(payload + 8, body, size);
    c->ready = 0;
    int error = -NLP_BUSY;
    for (unsigned i = 0; i < 500 && error == -NLP_BUSY; ++i) {
        error =
            nlp_session_send(&c->session, 1, NLP_REQUEST, op, 1, payload, size + 8, service.now_ms);
        if (error == -NLP_BUSY)
            link_drive();
    }
    assert(!error);
    for (unsigned i = 0; i < 500 && !c->ready; ++i)
        link_drive();
    assert(c->ready && c->response_size >= 8 && nlp_get32(c->response) == c->request);
    return nlp_get16(c->response + 4);
}
static void open_keymap(uint32_t proposed, unsigned permissions, unsigned expected_status)
{
    static const uint8_t type[16] = NLP_UUID_KEYMAP;
    uint8_t body[52] = {0};
    nlp_put32(body, proposed);
    nlp_put16(body + 4, 3);
    memcpy(body + 8, type, 16);
    nlp_put16(body + 24, 1);
    nlp_put32(body + 36, permissions);
    nlp_put16(body + 40, 128);
    body[42] = 1;
    body[43] = 2;
    body[44] = 1;
    nlp_put32(body + 48, 15000);
    assert(link_rpc(&link_clients[0], 14, body, 52) == expected_status);
    if (!expected_status)
        client_start(&link_clients[1], nlp_get32(link_clients[0].response + 8), 3,
                     service.link.endpoint.peer_node);
}
static void test_nlp_link(void)
{
    nlp_model = 1;
    memset(link_clients, 0, sizeof(link_clients));
    nlp_stream_init(&host_stream, NLP_FRAMING_TCP);
    setup();
    assert(service.link.ready);
    uint8_t body[120], value[32], uid[16] = {1};
    size_t size = 0;
#define ADD(tag, p, n) assert(!nlp_tlv_add(body, sizeof(body), &size, tag, p, n))
    nlp_put32(value, 123);
    ADD(1, value, 4);
    ADD(2, uid, 16);
    nlp_put64(value, 9);
    ADD(3, value, 8);
    value[0] = value[1] = 1;
    ADD(4, value, 2);
    nlp_put16(value, 128);
    ADD(5, value, 2);
    nlp_put16(value, 1);
    ADD(6, value, 2);
    ADD(7, value, 2);
    ADD(8, value, 2);
    send_control(1, body, size);
    size = 0;
    ADD(9, service.link.endpoint.config.fabric, 16);
    nlp_put32(value, service.link.endpoint.config.epoch);
    ADD(10, value, 4);
    ADD(13, service.link.endpoint.config.uid, 16);
    nlp_put32(value, 1);
    ADD(17, value, 4);
    send_control(3, body, size);
    assert(control_size >= 8 && service.link.endpoint.peer_node == 2);
    client_start(&link_clients[0], 1, 0, 2);
    open_keymap(2, 9, NLP_DENIED);
    open_keymap(2, 1, 0);
    assert(!link_rpc(&link_clients[1], 1, NULL, 0) &&
           nlp_get32(link_clients[1].response + 12) == 1056);
    nlp_put32(value, 2);
    assert(!link_rpc(&link_clients[0], 15, value, 4));
    nlp_session_close(&link_clients[1].session, NLP_DISCONNECTED);
    m.pressed = (UINT64_C(1) << 0) | (UINT64_C(1) << 58) | (UINT64_C(1) << 60);
    for (unsigned i = 0; i < 520; ++i)
        link_drive();
    assert(service.unlocked);
    open_keymap(3, 9, 0);
    struct kb_config changed = service.keyboard.config;
    changed.map[0][0] = (struct kb_action){KB_KEY, 0, KB_B};
    uint8_t encoded[NLP_KEYMAP_BYTES], begin[40] = {0};
    assert(!kb_config_encode(&changed, encoded, sizeof(encoded)));
    nlp_put32(begin, service.edits);
    nlp_put32(begin + 4, 1056);
    assert(!nlp_sha256(encoded, sizeof(encoded), begin + 8));
    assert(!link_rpc(&link_clients[1], 2, begin, 40));
    uint64_t tx = nlp_get64(link_clients[1].response + 8);
    for (unsigned offset = 0; offset < 1056;) {
        unsigned count = 1056 - offset;
        if (count > 104)
            count = 104;
        nlp_put64(body, tx);
        nlp_put32(body + 8, offset);
        nlp_put16(body + 12, (uint16_t)count);
        nlp_put16(body + 14, 0);
        memcpy(body + 16, encoded + offset, count);
        assert(!link_rpc(&link_clients[1], 4, body, 16 + count));
        offset += count;
    }
    nlp_put64(body, tx);
    assert(!link_rpc(&link_clients[1], 5, body, 8));
    memset(body + 8, 0, 8);
    assert(!link_rpc(&link_clients[1], 6, body, 16));
    uint32_t edits = service.edits;
    assert(service.keyboard.config.map[0][0].code == KB_B && service.dirty &&
           link_clients[1].response[12] == 0);
    assert(!link_rpc(&link_clients[1], 6, body, 16) && service.edits == edits);
    m.pressed = 0;
    for (unsigned i = 0; i < 10; ++i)
        link_drive();
    /* A new software peer boot cannot inherit the old 30-second physical grant. */
    nlp_session_close(&link_clients[0].session, NLP_DISCONNECTED);
    nlp_session_close(&link_clients[1].session, NLP_DISCONNECTED);
    size = 0;
    nlp_put32(value, 124);
    ADD(1, value, 4);
    ADD(2, uid, 16);
    nlp_put64(value, 10);
    ADD(3, value, 8);
    value[0] = value[1] = 1;
    ADD(4, value, 2);
    nlp_put16(value, 128);
    ADD(5, value, 2);
    nlp_put16(value, 1);
    ADD(6, value, 2);
    ADD(7, value, 2);
    ADD(8, value, 2);
    send_control(1, body, size);
    assert(service.link.endpoint.peer_boot == 10);
    size = 0;
    ADD(9, service.link.endpoint.config.fabric, 16);
    nlp_put32(value, service.link.endpoint.config.epoch);
    ADD(10, value, 4);
    ADD(13, service.link.endpoint.config.uid, 16);
    nlp_put32(value, 1);
    ADD(17, value, 4);
    send_control(3, body, size);
    assert(service.link.endpoint.peer_node == 3);
    client_start(&link_clients[0], 1, 0, 3);
    open_keymap(2, 9, NLP_DENIED);
#undef ADD
    nlp_model = 0;
    puts("PASS nk60_nlp_real_service_discovery_keymap_sha_commit_and_peer_bound_unlock");
}

int main(void)
{
    test_nlp_link();
    test_reports();
    test_backpressure();
    test_uart_security();
    test_persist_progress();
    test_macro_configuration();
    test_suspend_power_and_clock_wrap();
    printf("service_bytes=%zu\n", sizeof(service));
    return 0;
}
