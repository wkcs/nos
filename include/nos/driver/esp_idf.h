/* Copyright (C) 2026 NOS contributors. Board setup only; no ESP-IDF types. */
#ifndef NOS_DRIVER_ESP_IDF_H
#define NOS_DRIVER_ESP_IDF_H
#include "service_io.h"
#include "usb_otg.h"
/* Explicit board wiring; host power needs a protected default-off switch. */
struct nos_idf_usb_otg_config {
    int id_pin, vbus_sense_pin, vbus_enable_pin, overcurrent_pin;
    uint16_t debounce_ms, poll_ms, discharge_ms, power_timeout_ms;
    uint8_t allowed, source_allowed;
    uint8_t id_active_low, vbus_active_low, enable_active_low, overcurrent_active_low;
};
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
    const struct nos_ble_profile *ble_profile;
    const struct nos_idf_usb_otg_config *usb_otg;
};
int nos_idf_devices_init(void);
/* BLOCK instance 0 owns only the dedicated "storage" partition: raw subtype
 * 0x40 or explicitly selected IDF WL subtype 0x41. Formats never auto-migrate.
 * 512-byte logical sectors each occupy one 4096-byte erase block.
 * No unknown-media formatting, raw erase or access to NVS/OTA partitions. */
const char *nos_idf_storage_format(void);
/* Firmware-owned UART0: all terminal requests use the NOS service domain.
 * READ/WRITE take nos_io_buffer and are nonblocking, with short transfers.
 * The console cannot be stopped or taken over by application UART services. */
int nos_idf_console_call(uint32_t command, void *request, size_t size);
/* Create registers a STOPPED NOS device; start is explicit and may touch HW.
 * Configuration strings must outlive the device. No raw SDK handles escape. */
int nos_idf_device_create(const struct nos_idf_device_config *, struct nos_service **);
/* Stop errors keep *service and all claims intact, so callers can retry. */
int nos_idf_device_destroy(struct nos_service **);
int nos_idf_devices_snapshot(struct nos_service_info *, size_t capacity, size_t *count);
#endif
