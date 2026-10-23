/* Copyright (C) 2026 NOS contributors. */
#include "keyboard_usb.h"

#define EINVAL 22
#define EAGAIN 11
#define EACCES 13
#define ENODEV 19
#define EP_IN 0x81u
#define CTRL_IDLE 0u
#define CTRL_IN 1u
#define CTRL_STATUS_OUT 2u
#define CTRL_OUT 3u
#define CTRL_STATUS_IN 4u
#define CTRL_STALL 5u
#define ACTION_ADDRESS 1u
#define ACTION_CONFIG 2u
#define ACTION_LEDS 3u
#define ACTION_PROTOCOL 4u
#define ACTION_REMOTE 5u
#define ACTION_IDLE 6u
#define ACTION_HALT 7u
#define ACTION_INTERFACE 8u

/* A single boot-capable interface avoids two logical keyboards reporting the
 * same keys. Report protocol uses ID1 NKRO, ID2 consumer, or optional ID3 6KRO.
 * NKRO bit n represents usage n; E0..E7 are solely in the modifier byte. */
static const uint8_t report_descriptor[] = {
    0x05,0x01,0x09,0x06,0xa1,0x01,0x85,0x01,
    0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0x00,0x25,0x01,
    0x75,0x01,0x95,0x08,0x81,0x02,
    0x19,0x00,0x2a,0xff,0x00,0x75,0x01,0x96,0x00,0x01,0x81,0x02,
    0x05,0x08,0x19,0x01,0x29,0x05,0x75,0x01,0x95,0x05,0x91,0x02,
    0x75,0x03,0x95,0x01,0x91,0x03,0xc0,
    0x05,0x0c,0x09,0x01,0xa1,0x01,0x85,0x02,
    0x15,0x00,0x26,0xff,0x03,0x19,0x00,0x2a,0xff,0x03,
    0x75,0x10,0x95,0x01,0x81,0x00,0xc0,
    0x05,0x01,0x09,0x06,0xa1,0x01,0x85,0x03,
    0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0x00,0x25,0x01,
    0x75,0x01,0x95,0x08,0x81,0x02,
    0x75,0x08,0x95,0x01,0x81,0x03,
    0x19,0x00,0x2a,0xff,0x00,0x15,0x00,0x26,0xff,0x00,
    0x75,0x08,0x95,0x06,0x81,0x00,0xc0
};
/* Legacy NOS VID/PID are retained for development, not a new USB-IF grant. */
static const uint8_t device_descriptor[] = {
    18,1,0x00,0x02,0,0,0,64,0xff,0x0e,0x01,0x00,0x00,0x02,1,2,0,1
};
static const uint8_t configuration_descriptor[] = {
    9,2,34,0,1,1,0,0xa0,250,
    9,4,0,0,1,3,1,1,0,
    9,0x21,0x11,0x01,0,1,0x22,sizeof(report_descriptor),0,
    7,5,0x81,3,64,0,1
};
static void copy_bytes(uint8_t *to, const uint8_t *from, size_t length)
{
    size_t i;
    for (i = 0; i < length; ++i) to[i] = from[i];
}
static int equal_bytes(const uint8_t *a, const uint8_t *b, size_t length)
{
    size_t i;
    for (i = 0; i < length; ++i) if (a[i] != b[i]) return 0;
    return 1;
}
static void clear_bytes(void *pointer, size_t length)
{
    uint8_t *bytes = pointer;
    size_t i;
    for (i = 0; i < length; ++i) bytes[i] = 0;
}
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
const uint8_t *usb_keyboard_report_descriptor(size_t *length)
{
    if (length) *length = sizeof(report_descriptor);
    return report_descriptor;
}
static void stall_control(struct usb_keyboard *u)
{
    u->state = CTRL_STALL;
    u->control_busy = 0;
    u->action = 0;
    u->transport.stall(u->transport.context, 0, 1);
    u->transport.stall(u->transport.context, 0x80, 1);
}
static void pump_control(struct usb_keyboard *u)
{
    unsigned remaining, length;
    int result;
    if (u->control_busy) return;
    if (u->state == CTRL_STATUS_IN) {
        result = u->transport.send(u->transport.context, 0x80, u->control, 0);
    } else if (u->state == CTRL_IN) {
        remaining = u->control_length - u->control_offset;
        length = remaining > 64 ? 64 : remaining;
        if (!remaining) {
            if (!u->control_zlp || u->control_zlp_sent) {
                u->state = CTRL_STATUS_OUT;
                return;
            }
        }
        result = u->transport.send(u->transport.context, 0x80,
                                  u->control + u->control_offset, length);
        if (!result) {
            u->control_offset += (uint16_t)length;
            if (!length) u->control_zlp_sent = 1;
        }
    } else return;
    if (!result) u->control_busy = 1;
    else if (result != -EAGAIN) { u->last_error = result; stall_control(u); }
}
static void control_in(struct usb_keyboard *u, const uint8_t *bytes,
                       size_t length, uint16_t requested)
{
    if (length > requested) length = requested;
    if (length > sizeof(u->control)) { stall_control(u); return; }
    copy_bytes(u->control, bytes, length);
    u->control_length = (uint16_t)length;
    u->requested_length = requested;
    u->control_offset = 0;
    u->control_zlp = length < requested && !(length & 63u);
    u->control_zlp_sent = 0;
    /* A zero wLength has no data stage. */
    u->state = requested ? CTRL_IN : CTRL_STATUS_OUT;
    pump_control(u);
}
static void status_in(struct usb_keyboard *u, uint8_t action, uint8_t value)
{
    u->action = action;
    u->action_value = value;
    u->state = CTRL_STATUS_IN;
    pump_control(u);
}
static void drop_queue(struct usb_keyboard *u)
{
    u->transport.cancel(u->transport.context, EP_IN);
    u->head = u->count = u->in_busy = 0;
}
static void enqueue(struct usb_keyboard *u, const uint8_t *data, unsigned length,
                    unsigned id)
{
    struct usb_keyboard_packet *p = &u->queue[(u->head + u->count) % USB_KBD_QUEUE];
    p->length = (uint8_t)(length + (id ? 1 : 0));
    if (id) p->data[0] = (uint8_t)id;
    copy_bytes(p->data + (id ? 1 : 0), data, length);
    ++u->count;
}
static void enqueue_keyboard(struct usb_keyboard *u)
{
    if (!u->protocol) enqueue(u, u->boot, 8, 0);
    else if (u->nkro_enabled) enqueue(u, u->nkro, 33, 1);
    else enqueue(u, u->boot, 8, 3);
}
static void pump_interrupt(struct usb_keyboard *u)
{
    struct usb_keyboard_packet *p;
    int result;
    if (!u->configured || u->suspended || u->halted || u->in_busy) return;
    if (u->pending_sync && !u->count) {
        enqueue_keyboard(u);
        if (u->protocol) enqueue(u, u->consumer, 2, 2);
        u->pending_sync = 0;
    }
    if (!u->count) return;
    p = &u->queue[u->head];
    result = u->transport.send(u->transport.context, EP_IN, p->data, p->length);
    if (!result) u->in_busy = 1;
    else if (result != -EAGAIN) u->last_error = result;
}
void usb_keyboard_init(struct usb_keyboard *u, const struct usb_keyboard_transport *t)
{
    clear_bytes(u, sizeof(*u));
    u->transport = *t;
    u->protocol = 1;
    u->nkro_enabled = 1;
}
void usb_keyboard_reset(struct usb_keyboard *u)
{
    struct usb_keyboard_transport t = u->transport;
    u->transport.cancel(t.context, 0x80);
    u->transport.cancel(t.context, EP_IN);
    u->transport.configure(t.context, 0);
    u->transport.address(t.context, 0);
    u->transport.wake(t.context, 0);
    usb_keyboard_init(u, &t);
}
static void descriptor(struct usb_keyboard *u, uint16_t value, uint16_t index,
                       uint16_t length, uint8_t recipient)
{
    uint8_t string[64];
    const char *text = 0;
    unsigned i, n;
    if ((value >> 8) == 1 && !(value & 255) && !index && !recipient)
        control_in(u, device_descriptor, sizeof(device_descriptor), length);
    else if ((value >> 8) == 2 && !(value & 255) && !index && !recipient)
        control_in(u, configuration_descriptor, sizeof(configuration_descriptor), length);
    else if ((value >> 8) == 0x21 && !(value & 255) && !index && recipient == 1)
        control_in(u, configuration_descriptor + 18, 9, length);
    else if ((value >> 8) == 0x22 && !(value & 255) && !index && recipient == 1)
        control_in(u, report_descriptor, sizeof(report_descriptor), length);
    else if ((value >> 8) == 3 && !recipient) {
        if (!(value & 255) && !index) {
            static const uint8_t language[] = {4,3,9,4};
            control_in(u, language, sizeof(language), length);
            return;
        }
        if (index != 0x409) { stall_control(u); return; }
        if ((value & 255) == 1) text = "NOS";
        if ((value & 255) == 2) text = "NK60 v2";
        if (!text) { stall_control(u); return; }
        for (n = 0; text[n] && n < 31; ++n) { string[2 + n * 2] = (uint8_t)text[n]; string[3 + n * 2] = 0; }
        i = 2 + n * 2; string[0] = (uint8_t)i; string[1] = 3;
        control_in(u, string, i, length);
    } else stall_control(u);
}
void usb_keyboard_setup(struct usb_keyboard *u, const uint8_t s[8])
{
    uint8_t request = s[1], type = s[0], recipient = type & 31, bytes[34];
    uint16_t value = le16(s + 2), index = le16(s + 4), length = le16(s + 6);
    u->transport.cancel(u->transport.context, 0x80);
    u->transport.stall(u->transport.context, 0, 0);
    u->transport.stall(u->transport.context, 0x80, 0);
    u->state = CTRL_IDLE; u->action = u->control_busy = 0;
    u->output_received = 0;
    if ((type & 0x60) == 0) {
        if (request == 6 && (type == 0x80 || type == 0x81)) {
            descriptor(u, value, index, length, recipient); return;
        }
        if (request == 5 && type == 0 && !index && !length && value < 128 && !u->configured) {
            status_in(u, ACTION_ADDRESS, (uint8_t)value); return;
        }
        if (request == 9 && type == 0 && !index && !length && value <= 1 && u->address) {
            status_in(u, ACTION_CONFIG, (uint8_t)value); return;
        }
        if (request == 8 && type == 0x80 && !value && !index && length == 1 && u->address) {
            bytes[0] = u->configured; control_in(u, bytes, 1, length); return;
        }
        if (request == 0 && !(value) && length == 2 && (type & 0x80)) {
            bytes[0] = bytes[1] = 0;
            if (type == 0x80 && !index) bytes[0] = (uint8_t)(u->remote_enabled ? 2 : 0);
            else if (type == 0x81 && !index && u->configured) { }
            else if (type == 0x82 && (index == 0 || index == 0x80 || (index == EP_IN && u->configured)))
                bytes[0] = index == EP_IN ? u->halted : 0;
            else { stall_control(u); return; }
            control_in(u, bytes, 2, length); return;
        }
        if ((request == 1 || request == 3) && !length) {
            if (type == 0 && value == 1 && !index && u->configured) {
                status_in(u, ACTION_REMOTE, request == 3); return;
            }
            if (type == 2 && !value && index == EP_IN && u->configured) {
                status_in(u, ACTION_HALT, request == 3); return;
            }
        }
        if (request == 10 && type == 0x81 && !value && !index && length == 1 && u->configured) {
            bytes[0] = 0; control_in(u, bytes, 1, 1); return;
        }
        if (request == 11 && type == 1 && !value && !index && !length && u->configured) {
            status_in(u, ACTION_INTERFACE, 0); return;
        }
    } else if ((type == 0x21 || type == 0xa1) && !index && u->configured) {
        if (request == 0x0b && type == 0x21 && !length && value <= 1) {
            status_in(u, ACTION_PROTOCOL, (uint8_t)value); return;
        }
        if (request == 3 && type == 0xa1 && !value && length == 1) {
            bytes[0] = u->protocol; control_in(u, bytes, 1, 1); return;
        }
        if (request == 0x0a && type == 0x21 && !length &&
            (value & 255) <= (u->protocol ? 3u : 0u)) {
            u->action_id = (uint8_t)value;
            status_in(u, ACTION_IDLE, (uint8_t)(value >> 8)); return;
        }
        if (request == 2 && type == 0xa1 && !(value >> 8) &&
            (value & 255) <= (u->protocol ? 3u : 0u) && length == 1) {
            bytes[0] = u->idle_rate[value & 255]; control_in(u, bytes, 1, 1); return;
        }
        if (request == 9 && type == 0x21 && (value >> 8) == 2 &&
            ((!u->protocol && !(value & 255) && length == 1) ||
             (u->protocol && (value & 255) == 1 && length == 2))) {
            u->output_id = (uint8_t)value; u->output_length = (uint8_t)length;
            u->state = CTRL_OUT; return;
        }
        if (request == 1 && type == 0xa1) {
            if ((value >> 8) == 2 && ((!u->protocol && !(value & 255)) ||
                (u->protocol && (value & 255) == 1))) {
                bytes[0] = u->protocol ? 1 : u->leds;
                bytes[1] = u->leds;
                control_in(u, bytes, u->protocol ? 2 : 1, length); return;
            }
            if ((value >> 8) == 1) {
                if (!u->protocol && !(value & 255)) { control_in(u, u->boot, 8, length); return; }
                if (u->protocol && (value & 255) == 1) { bytes[0] = 1; copy_bytes(bytes + 1, u->nkro, 33); control_in(u, bytes, 34, length); return; }
                if (u->protocol && (value & 255) == 2) { bytes[0] = 2; copy_bytes(bytes + 1, u->consumer, 2); control_in(u, bytes, 3, length); return; }
                if (u->protocol && (value & 255) == 3) { bytes[0] = 3; copy_bytes(bytes + 1, u->boot, 8); control_in(u, bytes, 9, length); return; }
            }
        }
    }
    stall_control(u);
}
void usb_keyboard_out(struct usb_keyboard *u, const uint8_t *bytes, size_t length)
{
    if (u->state == CTRL_STATUS_OUT && !length) { u->state = CTRL_IDLE; return; }
    if (u->state != CTRL_OUT || !length || length > (size_t)(u->output_length - u->output_received)) {
        stall_control(u); return;
    }
    copy_bytes(u->control + u->output_received, bytes, length);
    u->output_received += (uint8_t)length;
    /* Every supported output is shorter than EP0 MPS: short packet ends stage. */
    if (u->output_received != u->output_length ||
        (u->output_id && u->control[0] != u->output_id)) { stall_control(u); return; }
    status_in(u, ACTION_LEDS, u->control[u->output_id ? 1 : 0] & 31u);
}
void usb_keyboard_in_complete(struct usb_keyboard *u, uint8_t endpoint)
{
    if (endpoint == EP_IN) {
        if (!u->in_busy) return;
        u->in_busy = 0;
        if (u->count) { u->head = (u->head + 1) % USB_KBD_QUEUE; --u->count; }
        u->idle_ms = u->now_ms;
        {
            unsigned id = u->protocol ? u->queue[(u->head + USB_KBD_QUEUE - 1) % USB_KBD_QUEUE].data[0] : 0;
            if (id < 4) u->idle_stamp[id] = u->now_ms;
        }
        pump_interrupt(u); return;
    }
    if (endpoint != 0x80 || !u->control_busy) return;
    u->control_busy = 0;
    if (u->state == CTRL_IN) { pump_control(u); return; }
    if (u->state != CTRL_STATUS_IN) return;
    u->state = CTRL_IDLE;
    switch (u->action) {
    case ACTION_ADDRESS:
        u->address = u->action_value;
        u->transport.address(u->transport.context, u->address); break;
    case ACTION_CONFIG:
        drop_queue(u); u->configured = u->action_value;
        u->halted = u->remote_enabled = 0;
        u->transport.configure(u->transport.context, u->configured);
        /* Current physical state is retained for reconnection synchronisation. */
        u->pending_sync = u->configured;
        u->idle_ms = u->now_ms; break;
    case ACTION_INTERFACE:
        drop_queue(u); u->halted = 0;
        u->transport.configure(u->transport.context, 1); u->pending_sync = 1;
        break;
    case ACTION_LEDS: u->leds = u->action_value; break;
    case ACTION_PROTOCOL:
        drop_queue(u); u->protocol = u->action_value;
        u->pending_sync = 1; break;
    case ACTION_REMOTE: u->remote_enabled = u->action_value; break;
    case ACTION_IDLE:
        {
            unsigned i;
            for (i = 0; i < 4; ++i) if (!u->action_id || i == u->action_id) {
                u->idle_rate[i] = u->action_value; u->idle_stamp[i] = u->now_ms;
            }
            u->idle = u->idle_rate[0]; u->idle_ms = u->now_ms;
        }
        break;
    case ACTION_HALT:
        u->halted = u->action_value;
        if (u->halted) { u->transport.cancel(u->transport.context, EP_IN); u->in_busy = 0; }
        u->transport.stall(u->transport.context, EP_IN, u->halted); break;
    default: break;
    }
    u->action = 0;
    pump_interrupt(u);
}
int usb_keyboard_submit(struct usb_keyboard *u, const uint8_t boot[8],
                        const uint8_t nkro[33], const uint8_t consumer[2])
{
    unsigned needed;
    int keyboard_changed, consumer_changed;
    if (!u || !boot || !nkro || !consumer) return -EINVAL;
    keyboard_changed = !equal_bytes(u->protocol && u->nkro_enabled ? u->nkro : u->boot,
                                   u->protocol && u->nkro_enabled ? nkro : boot,
                                   u->protocol && u->nkro_enabled ? 33 : 8);
    consumer_changed = !equal_bytes(u->consumer, consumer, 2);
    needed = (unsigned)keyboard_changed + (unsigned)(u->protocol && consumer_changed);
    if (u->configured && (needed > USB_KBD_QUEUE - u->count || u->pending_sync)) return -EAGAIN;
    copy_bytes(u->boot, boot, 8); copy_bytes(u->nkro, nkro, 33); copy_bytes(u->consumer, consumer, 2);
    if (!u->configured) return 0;
    if (keyboard_changed) enqueue_keyboard(u);
    if (u->protocol && consumer_changed) enqueue(u, u->consumer, 2, 2);
    pump_interrupt(u);
    return 0;
}
int usb_keyboard_set_nkro(struct usb_keyboard *u, int enabled)
{
    uint8_t zeros[33] = {0};
    if (!u) return -EINVAL;
    enabled = !!enabled;
    if (u->nkro_enabled == enabled) return 0;
    /* Release the previously selected report before changing report IDs. */
    if (u->configured && u->protocol) {
        if (u->pending_sync || USB_KBD_QUEUE - u->count < 2) return -EAGAIN;
        enqueue(u, zeros, u->nkro_enabled ? 33 : 8, u->nkro_enabled ? 1 : 3);
        u->nkro_enabled = (uint8_t)enabled; enqueue_keyboard(u); pump_interrupt(u);
    } else u->nkro_enabled = (uint8_t)enabled;
    return 0;
}
void usb_keyboard_poll(struct usb_keyboard *u, uint32_t now_ms)
{
    u->now_ms = now_ms;
    if (u->waking && (uint32_t)(now_ms - u->wake_ms) >= 2) {
        u->transport.wake(u->transport.context, 0); u->waking = 0;
    }
    pump_control(u);
    if (u->configured && !u->suspended && !u->count && !u->pending_sync) {
        unsigned id = u->protocol ? (u->nkro_enabled ? 1u : 3u) : 0u;
        if (u->idle_rate[id] && (uint32_t)(now_ms - u->idle_stamp[id]) >= (uint32_t)u->idle_rate[id] * 4u) {
            enqueue_keyboard(u); u->idle_stamp[id] = now_ms;
        }
        if (u->protocol && u->idle_rate[2] && (uint32_t)(now_ms - u->idle_stamp[2]) >= (uint32_t)u->idle_rate[2] * 4u) {
            enqueue(u, u->consumer, 2, 2); u->idle_stamp[2] = now_ms;
        }
    }
    pump_interrupt(u);
}
void usb_keyboard_suspend(struct usb_keyboard *u)
{
    if (!u->suspended) u->suspended_ms = u->now_ms;
    u->suspended = 1;
}
void usb_keyboard_resume(struct usb_keyboard *u)
{
    u->suspended = 0;
    if (u->waking) { u->transport.wake(u->transport.context, 0); u->waking = 0; }
    pump_interrupt(u);
}
int usb_keyboard_remote_wake(struct usb_keyboard *u)
{
    if (!u->configured || !u->suspended || !u->remote_enabled) return -EACCES;
    if (u->waking || (uint32_t)(u->now_ms - u->suspended_ms) < 5) return -EAGAIN;
    u->transport.wake(u->transport.context, 1); u->waking = 1; u->wake_ms = u->now_ms;
    return 0;
}
uint8_t usb_keyboard_leds(const struct usb_keyboard *u) { return u->leds; }
