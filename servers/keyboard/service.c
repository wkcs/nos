/* Copyright (C) 2026 NOS contributors. */
#include "service.h"
#include <string.h>

#define SERVICE_EINVAL 22
#define SERVICE_ENODEV 19
#define SERVICE_EAGAIN 11
#define SERVICE_UNLOCK_MASK ((UINT64_C(1) << 0) | (UINT64_C(1) << 58) | (UINT64_C(1) << 60))
#define SERVICE_UNLOCK_MS 2000u
#define SERVICE_UNLOCK_WINDOW_MS 30000u
#define SERVICE_RGB_MS 100u
#define SERVICE_RGB_LIMIT 16u

static void service_runtime(struct nk60_service *s);

static void output(struct nk60_service *s, const char *text)
{
    size_t length = strlen(text);
    if (!s->hooks.write)
        return;
    if (length > NK60_SERVICE_TX_BYTES - s->tx_count) {
        s->tx_dropped += (uint32_t)length;
        return;
    }
    while (length--) {
        unsigned tail = (s->tx_head + s->tx_count) % NK60_SERVICE_TX_BYTES;
        s->tx[tail] = *text++;
        ++s->tx_count;
    }
}

static void flush_output(struct nk60_service *s)
{
    /* At most two UART octets per step: no synchronous status-line stalls. */
    for (unsigned i = 0; i < 2u && s->tx_count; ++i) {
        if (s->hooks.write(s->hooks.context, s->tx + s->tx_head, 1))
            break;
        s->tx_head = (uint16_t)((s->tx_head + 1u) % NK60_SERVICE_TX_BYTES);
        --s->tx_count;
    }
}

static void number(struct nk60_service *s, uint32_t value)
{
    char text[11];
    size_t at = sizeof(text);

    text[--at] = 0;
    do {
        text[--at] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    output(s, text + at);
}

static uint32_t clock_update(struct nk60_service *s)
{
    uint32_t now = s->hardware.io.time_us(s->hardware.io.context);
    uint32_t delta = now - s->last_us;

    /* Divide deltas, not the wrapping microsecond counter itself. */
    s->last_us = now;
    s->now_ms += delta / 1000u;
    s->fraction_us += delta % 1000u;
    if (s->fraction_us >= 1000u) {
        ++s->now_ms;
        s->fraction_us -= 1000u;
    }
    return now;
}

static int store_read(void *context, uint32_t offset, void *bytes, size_t length)
{
    struct nk60_service *s = context;
    return nk60_flash_read(&s->hardware, offset, bytes, length);
}

static int store_erase(void *context, uint32_t offset, size_t length)
{
    struct nk60_service *s = context;
    if (length != NK60_FLASH_SECTOR_SIZE)
        return -SERVICE_EINVAL;
    return nk60_flash_erase_sector(&s->hardware, offset);
}

static int store_write(void *context, uint32_t offset, const void *bytes, size_t length)
{
    struct nk60_service *s = context;
    return nk60_flash_program(&s->hardware, offset, bytes, length);
}

static void mark_dirty(struct nk60_service *s)
{
    s->dirty = 1;
    s->last_edit_ms = s->now_ms;
    ++s->edits;
    s->rgb_pending = 1;
}

static void apply_config(struct nk60_service *s)
{
    if (!kb_reconfigure(&s->keyboard, &s->candidate)) {
        (void)usb_keyboard_set_nkro(&s->usb.keyboard, s->candidate.nkro);
        mark_dirty(s);
        output(s, "OK changed; save deferred\r\n");
    } else {
        output(s, "ERR invalid configuration\r\n");
    }
}

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

static int parse_uint(const char **input, uint32_t *result)
{
    const char *p = *input;
    uint32_t value = 0;
    unsigned digits = 0;

    while (*p == ' ' || *p == '\t')
        ++p;
    while (*p >= '0' && *p <= '9') {
        uint32_t digit = (uint32_t)(*p++ - '0');
        if (value > (UINT32_MAX - digit) / 10u)
            return -1;
        value = value * 10u + digit;
        ++digits;
    }
    if (!digits || (*p && *p != ' ' && *p != '\t'))
        return -1;
    *input = p;
    *result = value;
    return 0;
}

static int parse_end(const char *p)
{
    while (*p == ' ' || *p == '\t')
        ++p;
    return !*p;
}

static void status(struct nk60_service *s)
{
    output(s, "nk60_v2 keys=61 scan_us=4000 configured=");
    number(s, s->usb.keyboard.configured);
    output(s, " suspended="); number(s, s->usb.keyboard.suspended);
    output(s, " flash_bytes="); number(s, s->hardware.flash_bytes);
    output(s, " generation="); number(s, s->generation);
    output(s, " dirty="); number(s, s->dirty);
    output(s, " layers="); number(s, kb_active_layers(&s->keyboard));
    output(s, " scan_overruns="); number(s, s->scan_overruns);
    output(s, " scan_errors="); number(s, s->scan_errors);
    output(s, " report_overflows="); number(s, s->keyboard.overflows);
    output(s, " usb_errors="); number(s, s->usb_errors);
    output(s, " save_errors="); number(s, s->save_errors);
    output(s, " rgb_errors="); number(s, s->rgb_errors);
    output(s, " uart_dropped="); number(s, s->tx_dropped);
    output(s, " unlocked="); number(s, s->unlocked);
    output(s, "\r\n");
}

void nk60_service_command(struct nk60_service *s, const char *line)
{
    char command[12];
    const char *p = line;
    uint32_t a = 0, b = 0, c = 0, d = 0, e = 0;
    size_t length = 0;

    if (!s || !line)
        return;
    while (*p == ' ' || *p == '\t')
        ++p;
    while (*p && *p != ' ' && *p != '\t') {
        if (length + 1u >= sizeof(command)) {
            output(s, "ERR command\r\n");
            return;
        }
        command[length++] = *p++;
    }
    command[length] = 0;
    if (!length)
        return;
    if (same(command, "help") && parse_end(p)) {
        output(s, "status | lock | save | defaults | nkro 0/1 | rgb mode brightness\r\n");
        output(s, "bind layer key type parameter code | debounce ms\r\n");
        output(s, "macro id [usage ...] (0..3, up to 8 steps); bind type10 parameter=id code0\r\n");
        output(s, "Changes: hold Esc+PN+Fn 2 seconds to unlock UART for 30 seconds.\r\n");
        return;
    }
    if (same(command, "status") && parse_end(p)) {
        status(s);
        return;
    }
    if (same(command, "lock") && parse_end(p)) {
        s->unlocked = 0;
        if (s->unlock_held)
            s->unlock_held = 2;
        output(s, "OK locked\r\n");
        return;
    }
    if (s->unlocked && s->now_ms - s->unlocked_ms >= SERVICE_UNLOCK_WINDOW_MS)
        s->unlocked = 0;
    if (!s->unlocked) {
        output(s, "ERR locked; hold Esc+PN+Fn for 2 seconds\r\n");
        return;
    }
    if (same(command, "save") && parse_end(p)) {
        if (!s->flash_ready) {
            output(s, "ERR flash unavailable\r\n");
            return;
        }
        if (!s->dirty) {
            output(s, "OK no changes\r\n");
            return;
        }
        s->save_requested = 1;
        output(s, "OK save queued; release all keys\r\n");
        return;
    }
    s->candidate = s->keyboard.config;
    if (same(command, "defaults") && parse_end(p)) {
        kb_default_config(&s->candidate);
    } else if (same(command, "nkro") && !parse_uint(&p, &a) && parse_end(p) && a <= 1u) {
        s->candidate.nkro = (uint8_t)a;
    } else if (same(command, "debounce") && !parse_uint(&p, &a) && parse_end(p) && a <= UINT16_MAX) {
        s->candidate.debounce_ms = (uint16_t)a;
    } else if (same(command, "rgb") && !parse_uint(&p, &a) && !parse_uint(&p, &b) &&
               parse_end(p) && a <= UINT8_MAX && b <= UINT8_MAX) {
        s->candidate.rgb_mode = (uint8_t)a;
        s->candidate.brightness = (uint8_t)b;
    } else if (same(command, "macro")) {
        unsigned steps = 0;
        if (parse_uint(&p, &a) || a >= KB_MACROS) {
            output(s, "ERR macro index\r\n");
            return;
        }
        memset(s->candidate.macros[a], 0, sizeof(s->candidate.macros[a]));
        while (!parse_end(p)) {
            if (steps == KB_MACRO_STEPS || parse_uint(&p, &b) || b > UINT16_MAX) {
                output(s, "ERR macro steps\r\n");
                return;
            }
            s->candidate.macros[a][steps++] = (uint16_t)b;
        }
        s->candidate.macro_length[a] = (uint8_t)steps;
    } else if (same(command, "bind") && !parse_uint(&p, &a) && !parse_uint(&p, &b) &&
               !parse_uint(&p, &c) && !parse_uint(&p, &d) && !parse_uint(&p, &e) &&
               parse_end(p) && a < KB_LAYERS && b < KB_KEYS &&
               c <= UINT8_MAX && d <= UINT8_MAX && e <= UINT16_MAX) {
        s->candidate.map[a][b] = (struct kb_action){(uint8_t)c, (uint8_t)d, (uint16_t)e};
    } else {
        output(s, "ERR arguments\r\n");
        return;
    }
    apply_config(s);
}

static void maintenance(struct nk60_service *s)
{
    if (!s->hooks.read_byte)
        return;
    /* Bound UART work so a continuously writing peer cannot starve USB. */
    for (unsigned count = 0; count < 16u; ++count) {
        int byte = s->hooks.read_byte(s->hooks.context);
        if (byte < 0)
            break;
        if (byte == '\n' && s->swallow_lf) {
            s->swallow_lf = 0;
            continue;
        }
        s->swallow_lf = byte == '\r';
        if (byte == '\r' || byte == '\n') {
            if (s->line_overflow)
                output(s, "ERR line too long\r\n");
            else {
                s->line[s->line_length] = 0;
                nk60_service_command(s, s->line);
            }
            s->line_length = s->line_overflow = 0;
        } else if (byte == 8 || byte == 127) {
            if (s->line_length && !s->line_overflow)
                --s->line_length;
        } else if (byte >= 32 && byte <= 126) {
            if (s->line_length + 1u < NK60_SERVICE_LINE_BYTES && !s->line_overflow)
                s->line[s->line_length++] = (char)byte;
            else
                s->line_overflow = 1;
        }
    }
}

static void reports(struct nk60_service *s)
{
    const struct kb_report *report;
    for (unsigned count = 0; count < KB_REPORT_QUEUE; ++count) {
        report = kb_report_peek(&s->keyboard);
        if (!report)
            break;
        if (usb_keyboard_submit(&s->usb.keyboard, report->boot,
                                report->nkro, report->consumer))
            break;
        kb_report_pop(&s->keyboard);
    }
}

static void flash_progress(void *context)
{
    struct nk60_service *s = context;
    if (!s->usb_ready || s->progress_busy)
        return;
    s->progress_busy = 1;
    service_runtime(s);
    s->progress_busy = 0;
}

static void physical_unlock(struct nk60_service *s)
{
    if ((s->pressed & SERVICE_UNLOCK_MASK) == SERVICE_UNLOCK_MASK) {
        if (!s->unlock_held) {
            s->unlock_started_ms = s->now_ms;
            s->unlock_held = s->unlocked ? 2 : 1;
        } else if (s->unlock_held == 1 && !s->unlocked &&
                   s->now_ms - s->unlock_started_ms >= SERVICE_UNLOCK_MS) {
            s->unlocked = 1;
            s->unlock_held = 2;
            s->unlocked_ms = s->now_ms;
            output(s, "OK UART configuration unlocked for 30 seconds\r\n");
        }
    } else {
        s->unlock_held = 0;
    }
    if (s->unlocked && s->now_ms - s->unlocked_ms >= SERVICE_UNLOCK_WINDOW_MS) {
        s->unlocked = 0;
        if (s->unlock_held)
            s->unlock_held = 2;
    }
}

static void poll_usb(struct nk60_service *s)
{
    int error = nk60_usb_poll(&s->usb, s->now_ms);
    if (!error)
        return;
    ++s->usb_errors;
    s->usb_error = error;
    s->usb_ready = 0;
    s->usb.keyboard.configured = 0;
    s->usb.keyboard.suspended = 0;
    s->configured = s->suspended = 0;
    kb_release_all(&s->keyboard);
    s->keyboard.blocked = 1;
    if (s->rgb_ready)
        (void)nk60_rgb_set_power(&s->hardware, 0);
    output(s, "ERR USB controller stopped; reset required\r\n");
}

static void service_runtime(struct nk60_service *s)
{
    uint32_t now_us = clock_update(s);
    if (s->usb_ready) {
        poll_usb(s);
        if (s->usb_ready && s->usb.keyboard.last_error != s->usb_error) {
            s->usb_error = s->usb.keyboard.last_error;
            if (s->usb_error)
                ++s->usb_errors;
        }
        if (s->configured != s->usb.keyboard.configured ||
            s->suspended != s->usb.keyboard.suspended) {
            kb_release_all(&s->keyboard);
            s->keyboard.blocked = 1;
            s->configured = s->usb.keyboard.configured;
            s->suspended = s->usb.keyboard.suspended;
            s->wake_requested = 0;
            s->rgb_pending = 1;
            if (s->rgb_ready && (!s->configured || s->suspended))
                (void)nk60_rgb_set_power(&s->hardware, 0);
        }
    }
    if (s->matrix_ready && (int32_t)(now_us - s->next_scan_us) >= 0) {
        uint32_t started = now_us;
        unsigned layers = kb_active_layers(&s->keyboard);
        s->scan_overruns += (now_us - s->next_scan_us) / NK60_SERVICE_SCAN_US;
        s->next_scan_us = now_us + NK60_SERVICE_SCAN_US;
        if (nk60_matrix_scan(&s->hardware, &s->pressed)) {
            ++s->scan_errors;
            kb_release_all(&s->keyboard);
            s->keyboard.blocked = 1;
            s->pressed = 0;
        } else {
            clock_update(s);
            (void)kb_scan(&s->keyboard, s->pressed, s->now_ms);
            physical_unlock(s);
            if (!s->pressed)
                s->wake_requested = 0;
            if (s->suspended && s->pressed && !s->wake_requested &&
                !usb_keyboard_remote_wake(&s->usb.keyboard))
                s->wake_requested = 1;
            if (kb_take_local_events(&s->keyboard)) {
                (void)usb_keyboard_set_nkro(&s->usb.keyboard, s->keyboard.config.nkro);
                mark_dirty(s);
            }
            if (kb_active_layers(&s->keyboard) != layers)
                s->rgb_pending = 1;
        }
        now_us = clock_update(s);
        if (now_us - started >= NK60_SERVICE_SCAN_US) {
            ++s->scan_overruns;
            s->next_scan_us = now_us + NK60_SERVICE_SCAN_US;
        }
    }
    if (s->usb_ready) {
        if (s->usb.keyboard.nkro_enabled != s->keyboard.config.nkro)
            (void)usb_keyboard_set_nkro(&s->usb.keyboard, s->keyboard.config.nkro);
        reports(s);
        poll_usb(s);
        if (s->last_leds != usb_keyboard_leds(&s->usb.keyboard)) {
            s->last_leds = usb_keyboard_leds(&s->usb.keyboard);
            s->rgb_pending = 1;
        }
    }
}

static void lighting(struct nk60_service *s)
{
    uint8_t level;
    unsigned layers;
    int error;
    if (!s->rgb_ready || !s->hooks.dma_tx || !s->rgb_pending ||
        s->now_ms - s->last_rgb_ms < SERVICE_RGB_MS)
        return;
    s->last_rgb_ms = s->now_ms;
    s->rgb_pending = 0;
    if (!s->configured || s->suspended || !s->keyboard.config.rgb_mode) {
        error = nk60_rgb_set_power(&s->hardware, 0);
        if (error)
            ++s->rgb_errors;
        return;
    }
    level = (uint8_t)(((unsigned)s->keyboard.config.brightness * SERVICE_RGB_LIMIT) / 255u);
    layers = kb_active_layers(&s->keyboard);
    for (unsigned i = 0; i < NK60_LED_COUNT; ++i) {
        s->colors[i].r = s->keyboard.config.rgb_mode == 1 ||
                         (layers & (1u << KB_LAYER_FN)) ? level : 0;
        s->colors[i].g = s->keyboard.config.rgb_mode == 1 ||
                         (layers & (1u << KB_LAYER_PN)) ? level : 0;
        s->colors[i].b = level;
    }
    /* Legacy canvas position x=0,y=2 corresponds to the physical Caps key. */
    if (s->last_leds & 2u) {
        unsigned caps = nk60_canvas_to_led[2u * NK60_CANVAS_WIDTH];
        if (caps < NK60_LED_COUNT)
            s->colors[caps] = (struct nk60_rgb){SERVICE_RGB_LIMIT, SERVICE_RGB_LIMIT, SERVICE_RGB_LIMIT};
    }
    error = nk60_rgb_set_power(&s->hardware, 1);
    if (!error)
        error = nk60_rgb_encode(s->colors, NK60_LED_COUNT, s->wave, sizeof(s->wave));
    if (!error)
        error = nk60_rgb_write(&s->hardware, s->wave, sizeof(s->wave),
                               s->hooks.dma_tx, s->hooks.context);
    if (error)
        ++s->rgb_errors;
}

static void persist(struct nk60_service *s)
{
    uint32_t edits;
    int error;
    if (!s->flash_ready || s->saving || !s->dirty || s->pressed ||
        kb_report_peek(&s->keyboard) || s->keyboard.macro_running ||
        (!s->save_requested && s->now_ms - s->last_edit_ms < NK60_SERVICE_SAVE_MS))
        return;
    s->saving = 1;
    s->save_requested = 0;
    edits = s->edits;
    /* Stable snapshot: progress may scan local keys and change live config. */
    s->candidate = s->keyboard.config;
    error = kb_config_save(&s->store, s->store_base, &s->candidate, &s->generation);
    s->saving = 0;
    if (error) {
        ++s->save_errors;
        s->last_edit_ms = s->now_ms; /* Back off after a failed write. */
        output(s, "ERR save failed; prior committed record retained\r\n");
    } else {
        if (s->edits == edits)
            s->dirty = 0;
        output(s, "OK configuration committed\r\n");
    }
}

int nk60_service_init(struct nk60_service *s, const struct nk60_io *io,
                      const struct nk60_service_hooks *hooks)
{
    int error;
    if (!s || !io || !hooks)
        return -SERVICE_EINVAL;
    memset(s, 0, sizeof(*s));
    s->hooks = *hooks;
    error = nk60_hw_bind(&s->hardware, io);
    if (error)
        return error;
    s->last_us = io->time_us(io->context);
    s->next_scan_us = s->last_us;
    kb_default_config(&s->candidate);
    s->store = (struct kb_store){s, store_read, store_erase, store_write};
    error = nk60_flash_init(&s->hardware);
    if (!error && s->hardware.flash_bytes >= 2u * KB_CONFIG_SLOT_BYTES) {
        s->flash_ready = 1;
        s->store_base = s->hardware.flash_bytes - 2u * KB_CONFIG_SLOT_BYTES;
        (void)kb_config_load(&s->store, s->store_base, &s->candidate, &s->generation);
    } else {
        output(s, "WARN external flash unavailable; configuration is volatile\r\n");
    }
    if (kb_init(&s->keyboard, &s->candidate))
        return -SERVICE_EINVAL;
    s->matrix_ready = nk60_matrix_init(&s->hardware) == 0;
    s->rgb_ready = nk60_rgb_init(&s->hardware) == 0;
    if (s->rgb_ready)
        (void)nk60_rgb_set_power(&s->hardware, 0);
    else
        output(s, "WARN RGB unavailable\r\n");
    error = nk60_usb_init(&s->usb, io);
    s->usb_ready = error == 0;
    if (s->usb_ready)
        (void)usb_keyboard_set_nkro(&s->usb.keyboard, s->keyboard.config.nkro);
    else {
        ++s->usb_errors;
        output(s, "ERR USB initialization failed\r\n");
    }
    nk60_hw_set_progress(&s->hardware, flash_progress, s);
    s->rgb_pending = 1;
    output(s, "NOS nk60_v2 user keyboard service; UART115200 8N1; help\r\n");
    status(s);
    return s->matrix_ready && s->usb_ready ? 0 : -SERVICE_ENODEV;
}

void nk60_service_step(struct nk60_service *s)
{
    if (!s)
        return;
    service_runtime(s);
    maintenance(s);
    persist(s);
    lighting(s);
    flush_output(s);
}

#ifdef NOS_NK60_V2
#include "abi.h"
#include "nk60_device.h"

static uint32_t device_read32(void *unused, uintptr_t address)
{
    (void)unused;
    return (uint32_t)nos_syscall(NOS_SYS_DEVICE, NOS_NK60_READ32, address, 0);
}
static void device_write32(void *unused, uintptr_t address, uint32_t value)
{
    (void)unused;
    (void)nos_syscall(NOS_SYS_DEVICE, NOS_NK60_WRITE32, address, value);
}
static uint8_t device_read8(void *unused, uintptr_t address)
{
    (void)unused;
    return (uint8_t)nos_syscall(NOS_SYS_DEVICE, NOS_NK60_READ8, address, 0);
}
static void device_write8(void *unused, uintptr_t address, uint8_t value)
{
    (void)unused;
    (void)nos_syscall(NOS_SYS_DEVICE, NOS_NK60_WRITE8, address, value);
}
static uint32_t device_time_us(void *unused)
{
    return device_read32(unused, 0x40000024u);
}
static void device_delay_us(void *unused, uint32_t delay)
{
    uint32_t started = device_time_us(unused);
    for (uint32_t attempts = 0; attempts < 1000000u; ++attempts)
        if (device_time_us(unused) - started >= delay)
            return;
    static const char error[] = "ERR nk60_v2 microsecond clock stalled\r\n";
    (void)nos_syscall(NOS_SYS_WRITE, (uintptr_t)error, sizeof(error) - 1u, 0);
    (void)nos_syscall(NOS_SYS_EXIT, 1, 0, 0);
    for (;;)
        (void)nos_syscall(NOS_SYS_YIELD, 0, 0, 0);
}
static int device_read_byte(void *unused)
{
    (void)unused;
    return (int)nos_syscall(NOS_SYS_DEVICE, 0, 0, 0);
}
static int device_output(void *unused, const char *bytes, size_t length)
{
    (void)unused;
    if (!(nos_syscall(NOS_SYS_DEVICE, 5, 0, 0) & 4))
        return -SERVICE_EAGAIN;
    intptr_t result = nos_syscall(NOS_SYS_WRITE, (uintptr_t)bytes, length, 0);
    return result == (intptr_t)length ? 0 : result < 0 ? (int)result : -5;
}
static int device_dma(void *unused, uintptr_t destination, const uint8_t *bytes,
                       size_t length, uint32_t timeout_us)
{
    (void)unused;
    (void)timeout_us;
    if (destination != NK60_SPI2 + NK60_SPI_DR || length != NK60_RGB_WAVE_BYTES)
        return -SERVICE_EINVAL;
    return (int)nos_syscall(NOS_SYS_DEVICE, NOS_NK60_RGB_DMA, (uintptr_t)bytes, length);
}

void nos_keyboard_service(uintptr_t unused)
{
    static struct nk60_service service;
    const struct nk60_io io = {NULL, device_read32, device_write32,
                               device_read8, device_write8,
                               device_time_us, device_delay_us};
    const struct nk60_service_hooks hooks = {NULL, device_read_byte, device_output, device_dma};
    (void)unused;
    (void)nk60_service_init(&service, &io, &hooks);
    for (;;) {
        nk60_service_step(&service);
        (void)nos_syscall(NOS_SYS_YIELD, 0, 0, 0);
    }
}
#endif
