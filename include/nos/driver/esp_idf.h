/* Copyright (C) 2026 NOS contributors. Board setup only; no ESP-IDF types. */
#ifndef NOS_DRIVER_ESP_IDF_H
#define NOS_DRIVER_ESP_IDF_H
#include "service_io.h"
/* Logical IDs, not SDK enum values. UART: 1/2 (0 is boot console);
 * SPI: 2/3 (1 is boot flash); I2C: 0/1; other services: 0.
 * Pins: GPIO[pin], UART[tx,rx,rts,cts], SPI[sck,mosi,miso,cs], I2C[sda,scl].
 * Set unused pins to -1. GPIO service owns exactly one pin.
 * Boot flash/PSRAM pins 26..37 and console 43/44 are reserved. Board firmware
 * must explicitly choose safe pins; this API never guesses a board wiring. */
struct nos_idf_device_config {
    const char *name;
    enum nos_service_kind kind;
    unsigned instance;
    int pins[16];
    uint32_t hz;
    const char *storage_namespace;
    /* RGB display: pins[pclk,hsync,vsync,de,d0..d7], RGB565 over 8-bit bus. */
    uint16_t width, height;
    uint16_t hsync, hback, hfront, vsync, vback, vfront;
};
int nos_idf_devices_init(void);
/* Create registers a STOPPED NOS device; start is explicit and may touch HW.
 * Configuration strings must outlive the device. No raw SDK handles escape. */
int nos_idf_device_create(const struct nos_idf_device_config *, struct nos_service **);
/* Stop errors keep *service and all claims intact, so callers can retry. */
int nos_idf_device_destroy(struct nos_service **);
int nos_idf_devices_snapshot(struct nos_service_info *, size_t capacity, size_t *count);
#endif
