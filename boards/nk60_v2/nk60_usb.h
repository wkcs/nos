/* Copyright (C) 2026 NOS contributors. */
#ifndef NOS_NK60_USB_H
#define NOS_NK60_USB_H
#include "hardware.h"
#include "../../servers/usb/keyboard_usb.h"
#define NK60_USB_BASE ((uintptr_t)0x50000000u)
/* After init, never move or copy this controller/class object. */
struct nk60_usb {
    struct nk60_io io;
    struct nos_device controller_device;
    struct nos_usb_device_controller controller;
    struct usb_keyboard keyboard;
    uint8_t setup[8], out[64];
    uint8_t ready, setup_valid, tx_busy[3];
    uint8_t out_valid, out_length;
    uint8_t nlp_out[64], nlp_length, nlp_ready;
    uint32_t nlp_generation;
    int error;
};
/* Board clock and PA11/12 AF10 must already be configured; USB clock is 48MHz.
 * All host/device operation is polled in userspace, no kernel USB stack. */
int nk60_usb_init(struct nk60_usb *, const struct nk60_io *);
int nk60_usb_poll(struct nk60_usb *, uint32_t now_ms);
int nk60_usb_nlp_read(struct nk60_usb *, uint8_t *, size_t);
int nk60_usb_nlp_write(struct nk60_usb *, const uint8_t *, size_t);
void nk60_usb_disconnect(struct nk60_usb *);
#endif
