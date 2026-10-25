/* Copyright (C) 2026 NOS contributors. Portable service requests, no SDK ABI. */
#ifndef NOS_DRIVER_SERVICE_IO_H
#define NOS_DRIVER_SERVICE_IO_H
#include "service.h"
#include "peripheral.h"
#include "ble.h"
/* Commands are interpreted together with service.kind, not a wire protocol. */
enum nos_service_command {
    NOS_IO_CONFIGURE = 1,
    NOS_IO_READ,
    NOS_IO_WRITE,
    NOS_IO_POLL,
    NOS_IO_TRANSFER,
    NOS_IO_RECOVER,
    NOS_IO_COMMIT,
    NOS_IO_ERASE,
    NOS_IO_CONNECT,
    NOS_IO_DISCONNECT,
    NOS_IO_STATUS,
    NOS_IO_OTA_BEGIN,
    NOS_IO_OTA_FINISH,
    NOS_IO_OTA_ABORT,
    NOS_IO_BLE_ADVERTISE = 0x100,
    NOS_IO_BLE_ADVERTISE_STOP,
    NOS_IO_BLE_EVENT,
    NOS_IO_BLE_VALUE_GET,
    NOS_IO_BLE_VALUE_SET,
    NOS_IO_BLE_NOTIFY,
    NOS_IO_BLE_DISCONNECT,
    NOS_IO_BLE_PAIRING_RESPONSE,
    NOS_IO_BLE_SECURE,
    NOS_IO_USB_ATTACH = 0x200,
    NOS_IO_USB_DETACH,
    NOS_IO_USB_INFO,
    NOS_IO_USB_HID_OPEN,
    NOS_IO_USB_HID_READ,
    NOS_IO_USB_HID_LEDS,
    NOS_IO_USB_MSC_OPEN,
    NOS_IO_USB_ROLE,
    NOS_IO_USB_OTG_STATUS
};
struct nos_io_buffer {
    void *data;
    size_t size;
    size_t actual;
};
/* Console NOS_IO_POLL: readiness snapshot; a later transfer may be short. */
struct nos_io_console_status {
    uint32_t readable, writable;
};
struct nos_io_spi {
    struct nos_spi_config config;
    const struct nos_spi_segment *segments;
    size_t count;
    uint32_t timeout_us;
};
struct nos_io_i2c {
    const struct nos_i2c_message *messages;
    size_t count;
    uint32_t timeout_us;
};
struct nos_io_kv {
    const char *key;
    uint32_t value;
};
struct nos_io_wifi_config {
    char ssid[33];
    char password[65];
};
struct nos_io_wifi_status {
    uint8_t connected;
    int8_t rssi;
};
/* OTA begin selects the inactive slot; finish validates, but does not select
 * it for boot. Activation needs a separate production update policy. */
struct nos_io_ota_begin {
    size_t image_size;
};
struct nos_io_pwm {
    uint16_t duty_permille;
};
struct nos_io_stream {
    void *data;
    size_t size, actual;
    uint32_t timeout_us;
};
struct nos_io_pulse {
    uint16_t duration0, duration1;
    uint8_t level0, level1;
};
struct nos_io_wave {
    const struct nos_io_pulse *pulses;
    size_t count;
    uint32_t timeout_us;
};
struct nos_io_block {
    void *data;
    uint64_t sector;
    size_t count, actual;
};
struct nos_io_block_info {
    uint64_t sectors;
    uint32_t sector_size;
};
struct nos_io_rectangle {
    const void *pixels;
    uint16_t x, y, width, height;
};
#define NOS_CAN_EXTENDED 1u
#define NOS_CAN_REMOTE 2u
struct nos_io_can_frame {
    uint32_t identifier;
    uint8_t flags, length, data[8];
};
struct nos_io_can_status {
    uint32_t bus_errors, tx_pending, rx_pending;
    uint8_t bus_off;
};
#endif
