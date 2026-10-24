/* Copyright (C) 2026 NOS contributors. */
#ifndef NOS_DRIVER_USB_DWC2_H
#define NOS_DRIVER_USB_DWC2_H
#include "usb_host.h"
/* DMA-mode DWC2 host, one channel and one packet at a time, root FS/LS/HS.
 * BSP sets clocks/PHY/pinmux, supplies authorized MMIO and a 32-bit DMA mapping.
 * Does not drive an external VBUS switch. No hub split transactions or ISO. */
struct nos_usb_dwc2_io {
    void *context;
    uint32_t (*read32)(void *, uint32_t offset);
    void (*write32)(void *, uint32_t offset, uint32_t value);
    uint32_t (*time_us)(void *);
    void (*delay_us)(void *, uint32_t);
    int (*dma_map)(void *, void *buffer, size_t, uint32_t *bus_address);
    void (*dma_unmap)(void *, void *buffer, size_t, uint32_t bus_address);
    /* Mandatory even for coherent targets (explicit no-op is permitted).
     * for_device=1 publishes CPU writes/invalidates future IN; =0 acquires DMA
     * writes. Must implement platform cache maintenance and memory barriers. */
    void (*dma_sync)(void *, void *buffer, size_t, int for_device);
    /* 0 infers 48 MHz internal FS or 60 MHz HS; UTMI 30 MHz must be explicit. */
    uint32_t phy_clock_hz;
};
struct nos_usb_dwc2 {
    struct nos_usb_dwc2_io io;
    _Alignas(64) uint8_t bounce[1024];
    uint32_t dma_address;
    uint16_t max_packet;
    uint8_t mapped, started, faulted;
};
/* Failed stop retains mapping/object ownership until a later stop confirms
 * reset. Never free/move this object while mapped, even after an error. */
extern const struct nos_usb_host_ops nos_usb_dwc2_host_ops;
#endif
