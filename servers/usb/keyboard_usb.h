/* Copyright (C) 2026 NOS contributors. */
#ifndef NOS_KEYBOARD_USB_H
#define NOS_KEYBOARD_USB_H
#include <stddef.h>
#include <stdint.h>
#define USB_KBD_QUEUE 16u
#define USB_KBD_PACKET 64u
/* Callbacks return 0 or negative errno. send() copies/consumes its bytes before
 * returning; -EAGAIN leaves them untouched. cancel() discards an in-flight IN.
 * EP0 cancellation must also clear stale completion flags on a new SETUP. */
struct usb_keyboard_transport {
    void *context;
    int (*send)(void *, uint8_t endpoint, const uint8_t *, size_t);
    void (*cancel)(void *, uint8_t endpoint);
    void (*stall)(void *, uint8_t endpoint, int enabled);
    void (*configure)(void *, int enabled);
    void (*address)(void *, uint8_t address);
    void (*wake)(void *, int asserted);
};
struct usb_keyboard_packet { uint8_t data[34], length; };
struct usb_keyboard {
    struct usb_keyboard_transport transport;
    struct usb_keyboard_packet queue[USB_KBD_QUEUE];
    uint8_t control[256], boot[8], nkro[33], consumer[2];
    uint32_t now_ms, suspended_ms, wake_ms, idle_ms;
    uint32_t idle_stamp[4];
    uint8_t idle_rate[4], action_id;
    uint16_t control_length, control_offset, requested_length;
    uint8_t state, control_busy, control_zlp, control_zlp_sent;
    uint8_t action, action_value, output_id, output_length, output_received;
    uint8_t address, configured, protocol, nkro_enabled, leds, idle;
    uint8_t remote_enabled, suspended, waking, halted, in_busy;
    uint8_t head, count, pending_sync;
    int last_error;
};
void usb_keyboard_init(struct usb_keyboard *, const struct usb_keyboard_transport *);
void usb_keyboard_reset(struct usb_keyboard *);
void usb_keyboard_setup(struct usb_keyboard *, const uint8_t setup[8]);
void usb_keyboard_out(struct usb_keyboard *, const uint8_t *, size_t);
void usb_keyboard_in_complete(struct usb_keyboard *, uint8_t endpoint);
void usb_keyboard_poll(struct usb_keyboard *, uint32_t now_ms);
void usb_keyboard_suspend(struct usb_keyboard *);
void usb_keyboard_resume(struct usb_keyboard *);
/* Wakes only following an explicit caller request, host opt-in and >=5ms
 * suspend. poll() clears the resume signal after 2ms. */
int usb_keyboard_remote_wake(struct usb_keyboard *);
/* Atomic snapshot acceptance: -EAGAIN means caller must retain and retry it.
 * No press/release packet is replaced while waiting for the USB host. */
int usb_keyboard_submit(struct usb_keyboard *, const uint8_t boot[8],
                        const uint8_t nkro[33], const uint8_t consumer[2]);
int usb_keyboard_set_nkro(struct usb_keyboard *, int enabled);
uint8_t usb_keyboard_leds(const struct usb_keyboard *);
const uint8_t *usb_keyboard_report_descriptor(size_t *length);
#endif
