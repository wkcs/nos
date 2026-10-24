/* Copyright (C) 2026 NOS contributors. Enumeration-independent class transport. */
#ifndef NOS_DRIVER_USB_TRANSPORT_H
#define NOS_DRIVER_USB_TRANSPORT_H
#include "usb_host.h"
struct nos_usb_class_info {
    struct nos_usb_interface interfaces[NOS_USB_INTERFACES];
    struct nos_usb_endpoint endpoints[NOS_USB_ENDPOINTS];
    uint32_t max_transfer;
    uint8_t interface_count, endpoint_count;
};
/* The platform owns enumeration, address allocation, toggles and controller
 * retries. Each operation validates generation. No caller buffer may remain
 * referenced after return, including cancellation failure: use owned bounce
 * storage and quarantine the session until it is quiescent. */
struct nos_usb_transport_ops {
    int (*describe)(void *, uint32_t generation, struct nos_usb_class_info *);
    int (*control)(void *, uint32_t, const uint8_t setup[8], void *, size_t, size_t *, uint32_t);
    int (*transfer)(void *, uint32_t, uint8_t endpoint, void *, size_t, size_t *, uint32_t);
    int (*clear_halt)(void *, uint32_t, uint8_t endpoint);
};
struct nos_usb_transport {
    const struct nos_usb_transport_ops *ops;
    void *context;
};
#endif
