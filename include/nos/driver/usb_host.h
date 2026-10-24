/* Copyright (C) 2026 NOS contributors. */
#ifndef NOS_DRIVER_USB_HOST_H
#define NOS_DRIVER_USB_HOST_H
#include "peripheral.h"
#define NOS_USB_EPIPE 32
#define NOS_USB_CONFIG_MAX 512u
#define NOS_USB_INTERFACES 8u
#define NOS_USB_ENDPOINTS 16u
#define NOS_USB_PORT_CONNECTED 1u
#define NOS_USB_PORT_CHANGED 2u
#define NOS_USB_PORT_OVERCURRENT 4u
#define NOS_USB_PORT_ENABLED 8u
enum nos_usb_speed { NOS_USB_LOW = 1, NOS_USB_FULL, NOS_USB_HIGH };
enum nos_usb_transfer_type {
    NOS_USB_CONTROL,
    NOS_USB_ISOCHRONOUS,
    NOS_USB_BULK,
    NOS_USB_INTERRUPT
};
enum nos_usb_pid { NOS_USB_DATA0, NOS_USB_DATA1, NOS_USB_SETUP };
/* One transaction, no retries. A NAK returns EAGAIN; STALL returns EPIPE.
 * Success means an ACKed packet and <=length bytes. Packet buffer lifetime ends
 * at return, except a quarantined backend must retain only its OWN bounce DMA.
 * IN length may be less than max_packet; backend must guard packet overrun. */
struct nos_usb_packet {
    void *data;
    size_t length;
    uint16_t max_packet;
    uint8_t address, endpoint, speed, type, pid;
};
struct nos_usb_host_ops {
    int (*start)(void *);
    /* Stop all DMA/IRQ before success; failure keeps the controller owned. */
    int (*stop)(void *);
    /* Read and acknowledge connection change. Does not power external VBUS. */
    int (*port_status)(void *, uint32_t *flags);
    int (*port_reset)(void *, uint8_t *speed);
    int (*packet)(void *, const struct nos_usb_packet *, size_t *actual, uint32_t timeout_us);
    uint32_t (*time_us)(void *);
    void (*delay_us)(void *, uint32_t);
};
struct nos_usb_endpoint {
    uint32_t next_poll_us;
    uint16_t max_packet;
    uint8_t address, type, interval, interface_number, toggle, halted, polled;
};
struct nos_usb_interface {
    uint8_t number, class_code, subclass, protocol;
};
struct nos_usb_host {
    struct nos_controller controller;
    const struct nos_usb_host_ops *ops;
    struct nos_usb_endpoint endpoints[NOS_USB_ENDPOINTS];
    struct nos_usb_interface interfaces[NOS_USB_INTERFACES];
    uint8_t configuration[NOS_USB_CONFIG_MAX];
    uint32_t generation;
    uint16_t vendor, product, config_length, power_budget_ma;
    uint8_t running, faulted, configured, speed, ep0_size, address;
    uint8_t configuration_value, endpoint_count, interface_count;
};
/* One directly attached USB 1.x/2.0 root device per controller, no hub/TT.
 * Handle is its monotonically increasing generation; 0 is never valid.
 * Objects are zero-initialized once, immovable, serialized by owning service.
 * start retains a device reference until stop has quiesced the HCD. */
int nos_usb_host_start(struct nos_usb_host *);
int nos_usb_host_stop(struct nos_usb_host *);
int nos_usb_host_poll(struct nos_usb_host *, uint32_t *port_flags);
int nos_usb_host_enumerate(struct nos_usb_host *, uint32_t *handle);
int nos_usb_host_control(struct nos_usb_host *, uint32_t handle, const uint8_t setup[8], void *data,
                         size_t length, size_t *actual, uint32_t timeout_us);
int nos_usb_host_transfer(struct nos_usb_host *, uint32_t handle, uint8_t endpoint, void *data,
                          size_t length, size_t *actual, uint32_t timeout_us);
int nos_usb_host_clear_halt(struct nos_usb_host *, uint32_t handle, uint8_t endpoint);
/* Strict, side-effect-free-on-error descriptor parsing, also used at enumeration.
 * Requires speed to be set. Caller supplies staging host storage; don't call on
 * a live configured host. Only alternate setting 0 is activated. */
int nos_usb_host_parse_configuration(struct nos_usb_host *, const uint8_t *, size_t);
#endif
