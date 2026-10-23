/* Copyright (C) 2026 NOS contributors. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../servers/keyboard/service.h"

struct model {
    uint32_t now_us;
    uint64_t pressed;
    struct nk60_usb *usb;
    struct nk60_hw *hardware;
    uint8_t flash[2u * KB_CONFIG_SLOT_BYTES];
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
static struct nk60_service service;
const uint8_t nk60_canvas_to_led[NK60_CANVAS_WIDTH * NK60_CANVAS_HEIGHT] = {0};

static uint32_t io_read32(void *context, uintptr_t address)
{
    (void)context; (void)address;
    return 0;
}
static void io_write32(void *context, uintptr_t address, uint32_t value)
{
    (void)context; (void)address; (void)value;
}
static uint8_t io_read8(void *context, uintptr_t address)
{
    (void)context; (void)address;
    return 0;
}
static void io_write8(void *context, uintptr_t address, uint8_t value)
{
    (void)context; (void)address; (void)value;
}
static uint32_t io_time(void *context) { return ((struct model *)context)->now_us; }
static void io_delay(void *context, uint32_t us) { ((struct model *)context)->now_us += us; }
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
static int dma(void *context, uintptr_t destination, const uint8_t *bytes,
                size_t length, uint32_t timeout)
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
    memset(hw, 0, sizeof(*hw)); hw->io = *io; m.hardware = hw; return 0;
}
void nk60_hw_set_progress(struct nk60_hw *hw, void (*callback)(void *), void *context)
{
    hw->progress = callback; hw->progress_context = context;
}
int nk60_matrix_init(struct nk60_hw *hw) { hw->matrix_ready = 1; return 0; }
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
    hw->flash_ready = 1; hw->flash_bytes = sizeof(m.flash); return 0;
}
int nk60_flash_read(struct nk60_hw *hw, uint32_t offset, void *bytes, size_t length)
{
    (void)hw;
    assert(offset <= sizeof(m.flash) && length <= sizeof(m.flash) - offset);
    memcpy(bytes, m.flash + offset, length); return 0;
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
int nk60_rgb_init(struct nk60_hw *hw) { hw->rgb_ready = 1; return 0; }
int nk60_rgb_set_power(struct nk60_hw *hw, int enabled)
{
    hw->rgb_powered = (uint8_t)!!enabled; m.powered = !!enabled; return 0;
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
int nk60_rgb_write(struct nk60_hw *hw, const uint8_t *wave, size_t length,
                    nk60_dma_tx_fn callback, void *context)
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
static void usb_cancel(void *context, uint8_t endpoint) { (void)context; (void)endpoint; }
static void usb_stall(void *context, uint8_t endpoint, int enabled)
{ (void)context; (void)endpoint; (void)enabled; }
static void usb_configure(void *context, int enabled) { (void)context; (void)enabled; }
static void usb_address(void *context, uint8_t address) { (void)context; (void)address; }
static void usb_wake(void *context, int enabled)
{
    if (enabled)
        ++((struct model *)context)->wake_count;
}
int nk60_usb_init(struct nk60_usb *usb, const struct nk60_io *io)
{
    struct usb_keyboard_transport transport = {&m, usb_send, usb_cancel, usb_stall,
                                                usb_configure, usb_address, usb_wake};
    memset(usb, 0, sizeof(*usb)); usb->io = *io; m.usb = usb;
    usb_keyboard_init(&usb->keyboard, &transport); usb->ready = 1; return 0;
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
    memset(&m, 0, sizeof(m)); memset(m.flash, 0xff, sizeof(m.flash));
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
    setup(); steps(3);
    m.pressed = UINT64_C(1) << 29; steps(4);
    m.pressed = 0; steps(4);
    unsigned press = 0, release = 0;
    for (unsigned i = 0; i < m.packet_count; ++i) {
        if (packet_key(i, KB_A)) press = i + 1;
        if (press && i + 1 > press && m.packet_bytes[i] == 34 && !packet_key(i, KB_A)) release = i + 1;
    }
    assert(press && release > press);
    puts("PASS nk60_service_real_engine_usb_press_release");
}
static void test_backpressure(void)
{
    setup(); steps(3);
    m.refuse_in = 1;
    uint8_t boot[8] = {0}, nkro[33] = {0}, consumer[2] = {0};
    for (unsigned i = 0; i < USB_KBD_QUEUE; ++i) {
        nkro[2] = (uint8_t)(i + 1);
        assert(!usb_keyboard_submit(&service.usb.keyboard, boot, nkro, consumer));
    }
    m.pressed = UINT64_C(1) << 29; steps(4);
    assert(kb_report_peek(&service.keyboard));
    m.pressed = 0; steps(4);
    assert(kb_report_peek(&service.keyboard));
    m.refuse_in = 0; steps(30);
    assert(!kb_report_peek(&service.keyboard));
    unsigned press = 0, release = 0;
    for (unsigned i = 0; i < m.packet_count; ++i) {
        if (packet_key(i, KB_A)) press = i + 1;
        if (press && i + 1 > press && m.packet_bytes[i] == 34 && !packet_key(i, KB_A)) release = i + 1;
    }
    assert(press && release > press);
    puts("PASS nk60_service_usb_backpressure_retains_release");
}
static void unlock(void)
{
    m.pressed = (UINT64_C(1) << 0) | (UINT64_C(1) << 58) | (UINT64_C(1) << 60);
    steps(503); assert(service.unlocked);
    m.pressed = 0; steps(4);
}
static void test_uart_security(void)
{
    setup(); steps(3);
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
    nk60_service_command(&service, "lock"); steps(4);
    assert(!service.unlocked); /* Holding a consumed chord never rearms. */
    m.pressed = 0; steps(4); unlock();
    steps(7501); assert(!service.unlocked);
    m.refuse_uart = 1;
    unsigned before = service.tx_count;
    steps(1); assert(service.tx_count >= before);
    for (unsigned i = 0; i < 10; ++i)
        nk60_service_command(&service, "status");
    assert(service.tx_count <= NK60_SERVICE_TX_BYTES && service.tx_dropped);
    m.refuse_uart = 0; drain_output();
    char input[128];
    memset(input, 'x', 110);
    memcpy(input + 110, "\r\nstatus\r\n", 11);
    m.input = input; steps(10); drain_output();
    assert(!service.line_length && !service.line_overflow);
    assert(strstr(m.output, "ERR line too long"));
    assert(strstr(m.output, "ERR locked") && strstr(m.output, "OK changed"));
    puts("PASS nk60_service_physical_unlock_parser_and_nonblocking_uart");
}
static void test_persist_progress(void)
{
    setup(); steps(3); unlock();
    nk60_service_command(&service, "bind 0 29 2 0 5");
    steps(4); assert(!m.erases);
    nk60_service_command(&service, "save");
    m.change_during_save = 1;
    steps(1);
    assert(m.erases == 1 && service.generation == 1 && !service.dirty);
    assert(m.progress_scans >= 35 && m.progress_polls >= 70);
    struct kb_config loaded; uint32_t generation = 0;
    assert(!kb_config_load(&service.store, service.store_base, &loaded, &generation));
    assert(generation == 1 && loaded.map[0][29].code == KB_B);
    unsigned press = 0, release = 0;
    for (unsigned i = 0; i < m.packet_count; ++i) {
        if (packet_key(i, KB_B)) press = i + 1;
        if (press && i + 1 > press && m.packet_bytes[i] == 34 && !packet_key(i, KB_B)) release = i + 1;
    }
    assert(press && release > press); /* Key events survive the erase interval. */
    nk60_service_command(&service, "bind 0 29 2 0 6");
    m.erase_fails = 1; nk60_service_command(&service, "save"); steps(4);
    assert(service.save_errors == 1 && service.dirty && service.generation == 1);
    assert(!kb_config_load(&service.store, service.store_base, &loaded, &generation));
    assert(generation == 1 && loaded.map[0][29].code == KB_B);
    puts("PASS nk60_service_flash_progress_keeps_usb_and_scan_alive");
}
static void test_macro_configuration(void)
{
    setup(); steps(3); unlock();
    nk60_service_command(&service, "macro 0 4 5");
    assert(service.keyboard.config.macro_length[0] == 2);
    nk60_service_command(&service, "macro 0 4 5 6 7 8 9 10 11 12");
    assert(service.keyboard.config.macro_length[0] == 2);
    nk60_service_command(&service, "bind 0 29 10 0 0");
    steps(4); m.pressed = UINT64_C(1) << 29; steps(40);
    m.pressed = 0; steps(4);
    unsigned a = 0, b = 0;
    for (unsigned i = 0; i < m.packet_count; ++i) {
        if (packet_key(i, KB_A)) a = i + 1;
        if (packet_key(i, KB_B)) b = i + 1;
    }
    assert(a && b > a);
    nk60_service_command(&service, "macro 0");
    assert(!service.keyboard.config.macro_length[0]);
    puts("PASS nk60_service_uart_macro_configuration_executes");
}
static void test_suspend_power_and_clock_wrap(void)
{
    setup(); steps(3); unlock();
    nk60_service_command(&service, "rgb 1 255"); steps(30);
    assert(m.powered && m.dma_count);
    usb_keyboard_suspend(&service.usb.keyboard);
    nk60_service_step(&service); assert(!m.powered && service.keyboard.blocked);
    service.usb.keyboard.remote_enabled = 1;
    m.pressed = UINT64_C(1) << 29; steps(30); assert(m.wake_count == 1);
    m.pressed = 0; steps(4);
    m.pressed = UINT64_C(1) << 29; steps(4); assert(m.wake_count == 2);
    m.pressed = 0; steps(4);
    usb_keyboard_resume(&service.usb.keyboard); steps(30); assert(m.powered);
    service.usb.keyboard.configured = 0; nk60_service_step(&service); assert(!m.powered);
    m.now_us = UINT32_MAX - 500u; service.last_us = m.now_us;
    service.next_scan_us = m.now_us;
    uint32_t before = service.now_ms;
    m.now_us += 1500u; nk60_service_step(&service);
    assert(service.now_ms - before >= 1 && service.now_ms - before <= 2);
    service.usb.keyboard.configured = 1; m.usb_fault = 1;
    nk60_service_step(&service);
    assert(!service.usb_ready && service.usb_errors == 1 && !m.powered);
    puts("PASS nk60_service_suspend_rgb_budget_and_microsecond_wrap");
}
int main(void)
{
    test_reports(); test_backpressure(); test_uart_security();
    test_persist_progress(); test_macro_configuration(); test_suspend_power_and_clock_wrap();
    printf("service_bytes=%zu\n", sizeof(service));
    return 0;
}
