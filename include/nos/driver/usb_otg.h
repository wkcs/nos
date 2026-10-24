/* Copyright (C) 2026 NOS contributors. */
#ifndef NOS_DRIVER_USB_OTG_H
#define NOS_DRIVER_USB_OTG_H
#include "device.h"
enum nos_usb_role { NOS_USB_ROLE_NONE, NOS_USB_ROLE_DEVICE, NOS_USB_ROLE_HOST, NOS_USB_ROLE_AUTO };
enum nos_usb_otg_phase {
    NOS_USB_OTG_IDLE,
    NOS_USB_OTG_DISCHARGE,
    NOS_USB_OTG_POWER_WAIT,
    NOS_USB_OTG_ACTIVE,
    NOS_USB_OTG_FAULT
};
#define NOS_USB_ALLOW_DEVICE 1u
#define NOS_USB_ALLOW_HOST 2u
struct nos_usb_otg_inputs {
    uint8_t id_valid, id_grounded, vbus_present, source_allowed, overcurrent;
};
struct nos_usb_otg_ops {
    /* Successful stop must cancel transfers, quiesce DMA/IRQ and remove the
     * device pull-up or host session. Failure MUST keep its resources alive. */
    int (*stop)(void *, enum nos_usb_role);
    int (*start)(void *, enum nos_usb_role);
    /* External protected VBUS power switch, NOT just HPRT.PWR. */
    int (*set_vbus)(void *, int enabled);
};
/* Serialized policy, fixed storage. BSP/TCPC supplies debounced real inputs.
 * AUTO is classic ID/VBUS selection, not USB-PD, HNP, SRP, ADP or Type-C CC.
 * source_allowed must represent board wiring and power-contract authorization.
 * Inputs and polling timestamps use wrapping monotonic milliseconds. */
struct nos_usb_otg {
    const struct nos_usb_otg_ops *ops;
    void *context;
    uint32_t changed_ms, generation;
    uint16_t discharge_ms, power_timeout_ms;
    uint8_t allowed, requested, active, phase, sourcing, busy;
    int error;
};
int nos_usb_otg_request(struct nos_usb_otg *, enum nos_usb_role);
/* 0 settled, EAGAIN while waiting/blocked, other negative errno on fault. */
int nos_usb_otg_update(struct nos_usb_otg *, const struct nos_usb_otg_inputs *, uint32_t now_ms);
/* Explicit fault recovery: retry old-role stop and VBUS-off, then wait discharge.
 * Does not restart either role; request must be supplied again afterwards. */
int nos_usb_otg_recover(struct nos_usb_otg *, uint32_t now_ms);
#endif
