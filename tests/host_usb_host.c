/* Copyright (C) 2026 NOS contributors. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/usb_host.h"
#include "driver/usb_class.h"
#include "driver/usb_dwc2.h"
#include "driver/usb_otg.h"
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                                        \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static int probe(struct nos_device *d)
{
    (void)d;
    return 0;
}
static void remove_device(struct nos_device *d)
{
    (void)d;
}
static const struct nos_driver driver = {"test,usb", probe, remove_device, NULL, NULL};
static void activate(struct nos_device *d)
{
    struct nos_device *entries[1];
    struct nos_device_registry registry = {entries, 1, 0};
    *d = (struct nos_device){.name = "usb", .compatible = "test,usb"};
    CHECK(!nos_device_register(&registry, d) && !nos_device_bind(d, &driver));
}
static const uint8_t config_template[] = {
    9,    2,    57, 0,    2, 1,    0,  0x80, 50, 9,  4,    0, 0, 1, 3,    1, 1,  0, 9,
    0x21, 0x11, 1,  0,    1, 0x22, 63, 0,    7,  5,  0x81, 3, 8, 0, 10,   9, 4,  1, 0,
    2,    8,    6,  0x50, 0, 7,    5,  2,    2,  64, 0,    0, 7, 5, 0x82, 2, 64, 0, 0};
struct model {
    uint8_t config[57], setup[8], address, ctrl_toggle, ctrl_pending, toggles[32], report;
    uint32_t stamp, flags, bot_tag;
    size_t ctrl_offset, packets, data_remaining, cbws;
    int stalled_clock, nak, error_at, short_in, overrun, mscmode, bad_csw, stop_error, start_error;
};
static uint16_t u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}
static uint32_t u32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void le32(uint8_t *p, uint32_t n)
{
    p[0] = (uint8_t)n;
    p[1] = (uint8_t)(n >> 8);
    p[2] = (uint8_t)(n >> 16);
    p[3] = (uint8_t)(n >> 24);
}
static int model_start(void *context)
{
    struct model *m = context;
    m->flags = NOS_USB_PORT_CONNECTED | NOS_USB_PORT_CHANGED;
    return m->start_error;
}
static int model_stop(void *context)
{
    return ((struct model *)context)->stop_error;
}
static int model_status(void *context, uint32_t *flags)
{
    struct model *m = context;
    *flags = m->flags;
    m->flags &= ~NOS_USB_PORT_CHANGED;
    return 0;
}
static int model_reset(void *context, uint8_t *speed)
{
    struct model *m = context;
    m->address = 0;
    memset(m->toggles, 0, 32);
    *speed = NOS_USB_FULL;
    return 0;
}
static uint32_t model_now(void *context)
{
    struct model *m = context;
    if (!m->stalled_clock)
        m->stamp += 10;
    return m->stamp;
}
static void model_delay(void *context, uint32_t us)
{
    struct model *m = context;
    if (!m->stalled_clock)
        m->stamp += us;
}
static int model_packet(void *context, const struct nos_usb_packet *p, size_t *actual,
                        uint32_t timeout)
{
    struct model *m = context;
    unsigned index = (p->endpoint & 15) + ((p->endpoint & 0x80) ? 16 : 0);
    (void)timeout;
    m->packets++;
    *actual = 0;
    CHECK(p->address == m->address);
    if ((int)m->packets == m->error_at)
        return -NOS_DRV_ETIMEDOUT;
    if (m->nak)
        return -NOS_DRV_EAGAIN;
    if (!(p->endpoint & 15)) {
        if (p->pid == NOS_USB_SETUP) {
            CHECK(!p->endpoint && p->length == 8);
            memcpy(m->setup, p->data, 8);
            m->ctrl_offset = 0;
            m->ctrl_toggle = 1;
            m->ctrl_pending = 1;
            *actual = 8;
            return 0;
        }
        CHECK(m->ctrl_pending);
        if (!p->length) {
            CHECK(p->pid == NOS_USB_DATA1);
            CHECK(p->endpoint == (u16(m->setup + 6) && (m->setup[0] & 0x80) ? 0 : 0x80));
            if (m->setup[0] == 0 && m->setup[1] == 5)
                m->address = m->setup[2];
            if (m->setup[0] == 0 && m->setup[1] == 9)
                memset(m->toggles, 0, 32);
            if (m->setup[0] == 2 && m->setup[1] == 1) {
                unsigned ep = m->setup[4];
                m->toggles[(ep & 15) + ((ep & 0x80) ? 16 : 0)] = 0;
            }
            m->ctrl_pending = 0;
            return 0;
        }
        CHECK(p->pid == m->ctrl_toggle);
        m->ctrl_toggle ^= 1;
        if (p->endpoint & 0x80) {
            const uint8_t descriptor[18] = {18,   1,    0,    2, 0, 0, 0, 64, 0x34,
                                            0x12, 0x78, 0x56, 0, 1, 0, 0, 0,  1};
            if (m->setup[1] == 6 && m->setup[3] == 1)
                memcpy(p->data, descriptor + m->ctrl_offset, p->length);
            else if (m->setup[1] == 6 && m->setup[3] == 2)
                memcpy(p->data, m->config + m->ctrl_offset, p->length);
            else
                memset(p->data, 0, p->length);
        }
        m->ctrl_offset += p->length;
        *actual = p->length;
        return 0;
    }
    CHECK(p->pid == m->toggles[index]);
    m->toggles[index] ^= 1;
    *actual = p->length;
    if (m->overrun) {
        *actual = p->length + 1;
        return 0;
    }
    if (p->type == NOS_USB_INTERRUPT) {
        memset(p->data, 0, p->length);
        if (p->length >= 8)
            ((uint8_t *)p->data)[2] = m->report;
        return 0;
    }
    if (m->mscmode) {
        if (!(p->endpoint & 0x80) && p->length == 31 && u32(p->data) == 0x43425355) {
            const uint8_t *cbw = p->data;
            m->cbws++;
            m->bot_tag = u32(cbw + 4);
            m->data_remaining = u32(cbw + 8);
        } else if (!(p->endpoint & 0x80)) {
            CHECK(p->length <= m->data_remaining);
            m->data_remaining -= p->length;
        } else if (!m->data_remaining) {
            CHECK(p->length == 13);
            uint8_t *out = p->data;
            memset(out, 0, 13);
            le32(out, 0x53425355);
            le32(out + 4, m->bot_tag + (m->bad_csw ? 1 : 0));
        } else {
            memset(p->data, 0, p->length);
            m->data_remaining -= p->length;
        }
    } else if (p->endpoint & 0x80) {
        *actual = m->short_in ? p->length - 2 : p->length;
        memset(p->data, 0x5a, *actual);
    }
    return 0;
}
static const struct nos_usb_host_ops model_ops = {
    model_start, model_stop, model_status, model_reset, model_packet, model_now, model_delay};
static void core_and_classes(void)
{
    struct model m = {0};
    struct nos_device device;
    uint32_t handle, old, flags;
    size_t actual;
    struct nos_usb_host h = {
        .controller = {&device, &m, 65536, 0}, .ops = &model_ops, .power_budget_ma = 500};
    uint8_t data[512];
    struct nos_usb_hid hid;
    struct nos_usb_msc msc;
    memcpy(m.config, config_template, 57);
    activate(&device);
    CHECK(!nos_usb_host_start(&h) && device.users == 1);
    CHECK(nos_device_unbind(&device) == -NOS_DRV_EBUSY);
    CHECK(!nos_usb_host_enumerate(&h, &handle) && h.endpoint_count == 3 && h.vendor == 0x1234);
    CHECK(!nos_usb_hid_open(&hid, &h, handle, 0));
    m.report = 4;
    CHECK(!nos_usb_hid_read(&hid, data, 8, &actual) && actual == 8 && data[2] == 4);
    CHECK(nos_usb_hid_read(&hid, data, 8, &actual) == -NOS_DRV_EAGAIN);
    CHECK(!nos_usb_hid_set_leds(&hid, 2));
    CHECK(!nos_usb_host_transfer(&h, handle, 2, data, 128, &actual, 500000) && actual == 128);
    m.error_at = (int)m.packets + 2;
    CHECK(nos_usb_host_transfer(&h, handle, 2, data, 128, &actual, 500000) == -NOS_DRV_ETIMEDOUT &&
          actual == 64);
    m.error_at = 0;
    CHECK(!nos_usb_host_transfer(&h, handle, 2, data, 64, &actual, 500000));
    m.short_in = 1;
    CHECK(!nos_usb_host_transfer(&h, handle, 0x82, data, 128, &actual, 500000) && actual == 62);
    m.short_in = 0;
    m.nak = 1;
    m.stalled_clock = 1;
    size_t before = m.packets;
    CHECK(nos_usb_host_transfer(&h, handle, 2, data, 1, &actual, 500000) == -NOS_DRV_ETIMEDOUT);
    CHECK(m.packets - before == 100000 && !actual && !h.controller.busy && device.users == 1);
    m.nak = m.stalled_clock = 0;
    m.mscmode = m.bad_csw = 1;
    CHECK(nos_usb_msc_open(&msc, &h, handle, 1) == -NOS_DRV_EIO && msc.blocked);
    CHECK(!nos_usb_msc_recover(&msc) && !msc.blocked);
    msc.blocks = 2048;
    msc.block_size = 512;
    CHECK(nos_usb_msc_write(&msc, 4, 1, data) == -NOS_DRV_EIO && msc.blocked);
    before = m.cbws;
    CHECK(nos_usb_msc_write(&msc, 4, 1, data) == -NOS_DRV_EIO && m.cbws == before);
    CHECK(nos_usb_msc_read(&msc, 2048, 1, data) == -NOS_DRV_EINVAL);
    m.mscmode = 0;
    old = handle;
    m.flags |= NOS_USB_PORT_CHANGED;
    CHECK(nos_usb_host_poll(&h, &flags) == -NOS_DRV_ENODEV && !h.configured);
    CHECK(nos_usb_host_transfer(&h, old, 2, data, 1, &actual, 500000) == -NOS_DRV_ENODEV);
    CHECK(!nos_usb_host_enumerate(&h, &handle) && handle != old);
    CHECK(nos_usb_host_transfer(&h, old, 2, data, 1, &actual, 500000) == -NOS_DRV_ENODEV);
    m.stop_error = -NOS_DRV_EIO;
    CHECK(nos_usb_host_stop(&h) == -NOS_DRV_EIO && h.faulted && device.users == 1);
    m.stop_error = 0;
    CHECK(!nos_usb_host_stop(&h) && !device.users);
    CHECK(!nos_usb_host_start(&h));
    h.generation = UINT32_MAX;
    CHECK(nos_usb_host_enumerate(&h, &handle) == -NOS_DRV_ENOSPC && !handle);
    CHECK(!nos_usb_host_stop(&h));
    m.start_error = m.stop_error = -NOS_DRV_EIO;
    device.users = UINT16_MAX - 1;
    CHECK(nos_usb_host_start(&h) == -NOS_DRV_EIO && h.faulted && device.users == UINT16_MAX);
    nos_device_put(&device); /* Another owner releases one saturated reference. */
    m.start_error = m.stop_error = 0;
    CHECK(!nos_usb_host_stop(&h) && device.users == UINT16_MAX - 2);
    device.users = 0;
    CHECK(!nos_device_unbind(&device));
    puts("PASS "
         "usb_host_actual_enumeration_control_pid_bulk_partial_interrupt_stale_and_bot_recovery");
}
static void parser(void)
{
    struct nos_usb_host h = {.speed = NOS_USB_FULL, .power_budget_ma = 500};
    uint8_t config[512];
    uint32_t seed = 0x6d21a381;
    unsigned rejected = 0;
    CHECK(!nos_usb_host_parse_configuration(&h, config_template, 57));
    memcpy(config, config_template, 57);
    config[52] = 2; /* duplicate OUT endpoint */
    CHECK(nos_usb_host_parse_configuration(&h, config, 57) == -NOS_DRV_EINVAL);
    for (unsigned i = 0; i < 50000; i++) {
        size_t length;
        uint8_t before[sizeof(h)];
        for (unsigned j = 0; j < sizeof(config); j++) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            config[j] = (uint8_t)seed;
        }
        length = seed % 513;
        memcpy(before, &h, sizeof(h));
        if (nos_usb_host_parse_configuration(&h, config, length)) {
            CHECK(!memcmp(before, &h, sizeof(h)));
            rejected++;
        }
    }
    CHECK(rejected > 49000);
    h.speed = NOS_USB_LOW;
    {
        const uint8_t low[] = {9, 2, 25, 0, 1, 1, 0, 0x80, 50, 9, 4, 0, 0,
                               1, 3, 1,  1, 0, 7, 5, 0x81, 3,  8, 0, 10};
        CHECK(!nos_usb_host_parse_configuration(&h, low, sizeof(low)));
    }
    CHECK(nos_usb_host_parse_configuration(&h, config_template, 57) == -NOS_DRV_EINVAL);
    puts("PASS usb_host_descriptor_atomic_validation_50000_malformed_configs");
}
struct role_model {
    struct nos_usb_otg *otg;
    int role, power, start_error, stop_error, power_error;
    unsigned starts, stops;
};
static int role_start(void *context, enum nos_usb_role role)
{
    struct role_model *m = context;
    CHECK(!m->role);
    CHECK(role != NOS_USB_ROLE_HOST || m->power);
    CHECK(nos_usb_otg_request(m->otg, NOS_USB_ROLE_NONE) == -NOS_DRV_EBUSY);
    m->role = role;
    m->starts++;
    return m->start_error;
}
static int role_stop(void *context, enum nos_usb_role role)
{
    struct role_model *m = context;
    CHECK(m->role == (int)role);
    m->stops++;
    if (!m->stop_error)
        m->role = 0;
    return m->stop_error;
}
static int role_vbus(void *context, int on)
{
    struct role_model *m = context;
    if (!m->power_error)
        m->power = on;
    return m->power_error;
}
static const struct nos_usb_otg_ops role_ops = {role_stop, role_start, role_vbus};
static void otg(void)
{
    struct role_model m = {0};
    struct nos_usb_otg o = {.ops = &role_ops, .context = &m, .allowed = 3};
    struct nos_usb_otg_inputs in = {1, 0, 1, 1, 0};
    m.otg = &o;
    CHECK(!nos_usb_otg_request(&o, NOS_USB_ROLE_AUTO));
    CHECK(!nos_usb_otg_update(&o, &in, 0) && o.active == NOS_USB_ROLE_DEVICE && !m.power);
    in.id_grounded = 1;
    CHECK(nos_usb_otg_update(&o, &in, 10) == -NOS_DRV_EAGAIN && !m.role);
    CHECK(nos_usb_otg_update(&o, &in, 110) == -NOS_DRV_EAGAIN && !m.power); /* foreign VBUS */
    in.vbus_present = 0;
    CHECK(nos_usb_otg_update(&o, &in, 120) == -NOS_DRV_EAGAIN && m.power && !m.role);
    CHECK(nos_usb_otg_update(&o, &in, 121) == -NOS_DRV_EAGAIN);
    in.vbus_present = 1;
    CHECK(!nos_usb_otg_update(&o, &in, 130) && m.role == NOS_USB_ROLE_HOST);
    in.overcurrent = 1;
    CHECK(nos_usb_otg_update(&o, &in, 140) == -NOS_DRV_EIO && !m.power && !m.role &&
          o.phase == NOS_USB_OTG_FAULT);
    CHECK(nos_usb_otg_request(&o, NOS_USB_ROLE_DEVICE) == -NOS_DRV_EIO);
    in.overcurrent = 0;
    CHECK(!nos_usb_otg_recover(&o, UINT32_MAX - 50));
    CHECK(!nos_usb_otg_request(&o, NOS_USB_ROLE_DEVICE));
    CHECK(nos_usb_otg_update(&o, &in, 20) == -NOS_DRV_EAGAIN);
    m.start_error = -NOS_DRV_EIO;
    CHECK(nos_usb_otg_update(&o, &in, 60) == -NOS_DRV_EIO && !m.role &&
          o.phase == NOS_USB_OTG_FAULT);
    m.start_error = 0;
    CHECK(!nos_usb_otg_recover(&o, 100));
    CHECK(!nos_usb_otg_request(&o, NOS_USB_ROLE_HOST));
    in.vbus_present = 0;
    CHECK(nos_usb_otg_update(&o, &in, 200) == -NOS_DRV_EAGAIN && m.power);
    CHECK(nos_usb_otg_update(&o, &in, 301) == -NOS_DRV_ETIMEDOUT && !m.power);
    CHECK(!nos_usb_otg_recover(&o, 400));
    CHECK(!nos_usb_otg_request(&o, NOS_USB_ROLE_DEVICE));
    in.vbus_present = 1;
    CHECK(!nos_usb_otg_update(&o, &in, 500));
    m.stop_error = -NOS_DRV_ETIMEDOUT;
    CHECK(!nos_usb_otg_request(&o, NOS_USB_ROLE_HOST));
    CHECK(nos_usb_otg_update(&o, &in, 600) == -NOS_DRV_ETIMEDOUT && o.phase == NOS_USB_OTG_FAULT &&
          m.role == NOS_USB_ROLE_DEVICE);
    CHECK(nos_usb_otg_recover(&o, 700) == -NOS_DRV_ETIMEDOUT);
    m.stop_error = 0;
    CHECK(!nos_usb_otg_recover(&o, 800) && !m.role);
    o.allowed = NOS_USB_ALLOW_DEVICE;
    CHECK(nos_usb_otg_request(&o, NOS_USB_ROLE_HOST) == -NOS_DRV_ENOTSUP);
    puts("PASS "
         "usb_otg_id_vbus_roles_no_backfeed_discharge_wrap_overcurrent_timeout_and_failed_stop");
}
struct reg_model {
    uint32_t r[0x600 / 4], stamp;
    unsigned maps, unmaps;
    int mode, stuck_reset, stuck_halt, stalled;
    uint8_t *bounce;
};
static uint32_t reg_read(void *context, uint32_t offset)
{
    struct reg_model *m = context;
    CHECK(offset < 0x600 && !(offset & 3));
    return m->r[offset / 4];
}
static void reg_write(void *context, uint32_t offset, uint32_t value)
{
    struct reg_model *m = context;
    CHECK(offset < 0x600 && !(offset & 3));
    if (offset == 0x10) {
        m->r[4] = 0x80000000u | (m->stuck_reset ? value : 0);
        return;
    }
    if (offset == 0x508) {
        m->r[offset / 4] &= ~value;
        return;
    }
    if (offset == 0x500 && (value & (1u << 30))) {
        if (!m->stuck_halt) {
            m->r[offset / 4] = value & ~(3u << 30);
            m->r[0x508 / 4] |= 2;
        }
        return;
    }
    if (offset == 0x440 && (m->r[offset / 4] & (1u << 8)) && !(value & (1u << 8)))
        value |= 4;
    m->r[offset / 4] = value;
    if (offset == 0x500 && (value & (1u << 31))) {
        if (m->mode == 1) {
            m->r[0x508 / 4] = 16;
            return;
        }
        if (m->mode == 2) {
            m->r[0x508 / 4] = 10;
            m->r[offset / 4] &= ~(1u << 31);
            return;
        }
        if (m->mode == 3)
            return;
        m->r[0x508 / 4] = 3;
        m->r[offset / 4] &= ~(1u << 31);
        size_t size = m->r[0x510 / 4] & 0x7ffff;
        if (value & (1u << 15))
            memset(m->bounce, 0x42, size);
        m->r[0x510 / 4] &= ~0x7ffffu;
    }
}
static uint32_t reg_now(void *context)
{
    struct reg_model *m = context;
    if (!m->stalled)
        m->stamp += 10;
    return m->stamp;
}
static void reg_delay(void *context, uint32_t us)
{
    struct reg_model *m = context;
    if (!m->stalled)
        m->stamp += us;
}
static int reg_map(void *context, void *data, size_t count, uint32_t *address)
{
    struct reg_model *m = context;
    CHECK(count == 1024);
    m->bounce = data;
    m->maps++;
    *address = 0x1000;
    return 0;
}
static void reg_unmap(void *context, void *data, size_t count, uint32_t address)
{
    struct reg_model *m = context;
    CHECK(data == m->bounce && count == 1024 && address == 0x1000);
    m->unmaps++;
}
static void reg_sync(void *context, void *data, size_t count, int direction)
{
    struct reg_model *m = context;
    CHECK(data == m->bounce && count == 1024 && direction <= 1);
}
static void dwc2(void)
{
    struct reg_model m = {0};
    struct nos_device device;
    struct nos_usb_dwc2 d = {
        .io = {&m, reg_read, reg_write, reg_now, reg_delay, reg_map, reg_unmap, reg_sync, 0}};
    struct nos_usb_host h = {.controller = {&device, &d, 65536, 0}, .ops = &nos_usb_dwc2_host_ops};
    struct nos_usb_packet packet = {NULL,         8, 8, 1, 0x81, NOS_USB_FULL, NOS_USB_INTERRUPT,
                                    NOS_USB_DATA1};
    uint8_t bytes[8];
    size_t actual;
    activate(&device);
    m.r[0x40 / 4] = 0x4f54294a;
    m.r[0x48 / 4] = 2u << 3;
    m.r[0x4c / 4] = 4096u << 16;
    m.r[4] = 1u << 31;
    m.r[0x14 / 4] = 1;
    m.r[0x440 / 4] = (1u << 17) | 1;
    CHECK(!nos_usb_host_start(&h) && m.maps == 1 && d.mapped);
    packet.data = bytes;
    {
        uint8_t speed;
        m.r[0x440 / 4] = (2u << 17) | 1;
        CHECK(!h.ops->port_reset(&d, &speed) && speed == NOS_USB_LOW && m.r[0x400 / 4] == 2 &&
              m.r[0x404 / 4] == 6000);
        packet.speed = NOS_USB_LOW;
        CHECK(!h.ops->packet(&d, &packet, &actual, 1000) && (m.r[0x500 / 4] & (1u << 17)));
        packet.speed = NOS_USB_FULL;
        m.r[0x440 / 4] = (1u << 17) | 1;
        d.io.phy_clock_hz = 48000000;
        CHECK(!h.ops->port_reset(&d, &speed) && speed == NOS_USB_FULL && m.r[0x400 / 4] == 1 &&
              m.r[0x404 / 4] == 48000);
        d.io.phy_clock_hz = 0;
    }
    CHECK(!h.ops->packet(&d, &packet, &actual, 1000) && actual == 8 && bytes[0] == 0x42);
    CHECK((m.r[0x510 / 4] >> 29) == 2); /* DATA1 hardware PID */
    m.mode = 2;
    CHECK(h.ops->packet(&d, &packet, &actual, 1000) == -NOS_USB_EPIPE);
    m.mode = 1;
    CHECK(h.ops->packet(&d, &packet, &actual, 1000) == -NOS_DRV_EAGAIN && !d.faulted);
    m.mode = 3;
    m.stalled = 1;
    CHECK(h.ops->packet(&d, &packet, &actual, 1000) == -NOS_DRV_ETIMEDOUT && !d.faulted);
    m.mode = 1;
    m.stuck_halt = 1;
    CHECK(h.ops->packet(&d, &packet, &actual, 1000) == -NOS_DRV_EIO && d.faulted && d.mapped &&
          !m.unmaps);
    CHECK(h.ops->packet(&d, &packet, &actual, 1000) == -NOS_DRV_EIO);
    m.stuck_reset = 1;
    CHECK(nos_usb_host_stop(&h) == -NOS_DRV_ETIMEDOUT && device.users == 1 && !m.unmaps);
    CHECK(nos_device_unbind(&device) == -NOS_DRV_EBUSY);
    m.stuck_reset = m.stuck_halt = 0;
    CHECK(!nos_usb_host_stop(&h) && !device.users && m.unmaps == 1 && !d.mapped);
    m.r[0x48 / 4] = 0;
    CHECK(nos_usb_host_start(&h) == -NOS_DRV_ENOTSUP && !device.users && !d.mapped);
    m.r[0x48 / 4] = 2u << 3;
    m.r[0x4c / 4] = 95u << 16;
    CHECK(nos_usb_host_start(&h) == -NOS_DRV_ENOSPC && !device.users);
    m.r[0x4c / 4] = 96u << 16;
    CHECK(!nos_usb_host_start(&h) && d.max_packet == 64 && m.r[0x24 / 4] == 48);
    packet.max_packet = 512;
    CHECK(h.ops->packet(&d, &packet, &actual, 1000) == -NOS_DRV_ENOTSUP);
    CHECK(!nos_usb_host_stop(&h) && m.maps == 2 && m.unmaps == 2);
    CHECK(!nos_device_unbind(&device));
    puts("PASS usb_dwc2_dma_register_pid_nak_stall_stalled_clock_quarantine_and_confirmed_stop");
}
int main(void)
{
    core_and_classes();
    parser();
    otg();
    dwc2();
    return 0;
}
