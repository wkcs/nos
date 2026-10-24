/* Copyright (C) 2026 NOS contributors. Portable managed USB class requests. */
#ifndef NOS_DRIVER_USB_SERVICE_H
#define NOS_DRIVER_USB_SERVICE_H
#include "usb_class.h"
#include "usb_otg.h"
struct nos_usb_attach {
    uint32_t generation;
    uint8_t address;
};
struct nos_usb_class_open {
    uint32_t generation;
    uint8_t interface_number;
};
struct nos_usb_report {
    void *data;
    size_t capacity, actual;
    uint32_t generation;
};
struct nos_usb_leds {
    uint32_t generation;
    uint8_t leds;
};
struct nos_usb_storage_io {
    void *data;
    uint64_t sector;
    size_t count, actual;
    uint32_t generation;
};
struct nos_usb_device_info {
    struct nos_usb_class_info classes;
    uint32_t generation, blocks, block_size;
    uint16_t vendor, product;
    uint8_t address, speed, connected;
};
struct nos_usb_role_status {
    struct nos_usb_otg_inputs inputs;
    uint32_t generation;
    int32_t error;
    uint8_t requested, active, phase, sourcing;
};
#endif
