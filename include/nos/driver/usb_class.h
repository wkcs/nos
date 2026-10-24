/* Copyright (C) 2026 NOS contributors. */
#ifndef NOS_DRIVER_USB_CLASS_H
#define NOS_DRIVER_USB_CLASS_H
#include "usb_host.h"
#include "usb_transport.h"
struct nos_usb_hid {
    struct nos_usb_host *host;
    uint32_t handle;
    uint8_t endpoint, interface_number, protocol;
    struct nos_usb_transport io;
};
/* Boot keyboard(protocol=1)/mouse(2), interface_number selects a composite
 * function. Reports stay raw; applications decide how to route them to NLP/UI. */
int nos_usb_hid_open(struct nos_usb_hid *, struct nos_usb_host *, uint32_t handle,
                     uint8_t interface_number);
int nos_usb_hid_read(struct nos_usb_hid *, void *, size_t capacity, size_t *actual);
int nos_usb_hid_set_leds(struct nos_usb_hid *, uint8_t leds);
int nos_usb_hid_open_transport(struct nos_usb_hid *, const struct nos_usb_transport *, uint32_t,
                               uint8_t interface_number);
struct nos_usb_msc {
    struct nos_usb_host *host;
    uint32_t handle, tag, blocks, block_size;
    uint8_t interface_number, in_endpoint, out_endpoint, blocked;
    struct nos_usb_transport io;
    uint32_t max_transfer;
};
/* SCSI transparent + Bulk Only Transport, LUN0, READ/WRITE(10), <=32-bit LBA.
 * No UAS, filesystem, automatic replay or promise of power-loss persistence. */
int nos_usb_msc_open(struct nos_usb_msc *, struct nos_usb_host *, uint32_t handle,
                     uint8_t interface_number);
int nos_usb_msc_read(struct nos_usb_msc *, uint32_t lba, uint16_t blocks, void *);
int nos_usb_msc_write(struct nos_usb_msc *, uint32_t lba, uint16_t blocks, const void *);
int nos_usb_msc_sync(struct nos_usb_msc *);
int nos_usb_msc_recover(struct nos_usb_msc *);
int nos_usb_msc_open_transport(struct nos_usb_msc *, const struct nos_usb_transport *, uint32_t,
                               uint8_t interface_number);
#endif
