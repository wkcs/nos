/* Copyright (C) 2026 NOS contributors. */
#ifndef NOS_DRIVER_PERIPHERAL_H
#define NOS_DRIVER_PERIPHERAL_H
#include "device.h"
/* Synchronous bounded controller contracts. No buffer may outlive a callback.
 * Zero-copy/DMA is a backend detail: pin/map/sync/stop before returning, even on
 * timeout. busy protects against reentry, NOT inter-thread mutual exclusion.
 * Services with multiple callers must serialize on their IPC/lock boundary. */
struct nos_controller {
    struct nos_device *device;
    void *context;
    uint32_t max_transfer;
    uint8_t busy;
};
int nos_controller_begin(struct nos_controller *);
void nos_controller_end(struct nos_controller *);

/* GPIO numbers are controller-local, polarity applies to data and initial
 * output value. Pinmux/clock ownership is configured by the platform probe. */
enum nos_gpio_direction { NOS_GPIO_INPUT, NOS_GPIO_OUTPUT };
enum nos_gpio_pull { NOS_GPIO_PULL_NONE, NOS_GPIO_PULL_UP, NOS_GPIO_PULL_DOWN };
struct nos_gpio_config {
    uint8_t direction, pull, open_drain, initial_value;
};
struct nos_gpio_ops {
    int (*configure)(void *, uint32_t pin, const struct nos_gpio_config *);
    int (*read)(void *, uint32_t pin, int *physical_value);
    int (*write)(void *, uint32_t pin, int physical_value);
};
struct nos_gpio_controller {
    struct nos_controller controller;
    const struct nos_gpio_ops *ops;
    uint32_t pin_count;
};
struct nos_gpio {
    struct nos_gpio_controller *controller;
    uint32_t pin;
    uint8_t active_low;
};
int nos_gpio_configure(const struct nos_gpio *, const struct nos_gpio_config *);
int nos_gpio_read(const struct nos_gpio *, int *value);
int nos_gpio_write(const struct nos_gpio *, int value);

/* Whole message owns the bus, configures once, asserts CS once, then executes
 * segments without a CS gap. Null tx clocks tx_fill; null rx discards input.
 * Backend MUST deassert CS, stop DMA, drain/reset on every return path. Timeout
 * covers the WHOLE message, including controller drain, not each byte. */
struct nos_spi_config {
    uint32_t hz;
    uint16_t chip_select;
    uint8_t mode, bits, lsb_first;
};
struct nos_spi_segment {
    const void *tx;
    void *rx;
    size_t length;
    uint8_t tx_fill;
};
struct nos_spi_ops {
    int (*message)(void *, const struct nos_spi_config *, const struct nos_spi_segment *,
                   size_t count, uint32_t timeout_us);
};
struct nos_spi_bus {
    struct nos_controller controller;
    const struct nos_spi_ops *ops;
    uint32_t max_hz;
    uint16_t chip_select_count;
    uint8_t mode_mask;
};
struct nos_spi_device {
    struct nos_spi_bus *bus;
    struct nos_spi_config config;
};
int nos_spi_transfer(const struct nos_spi_device *, const struct nos_spi_segment *, size_t count,
                     uint32_t timeout_us);

/* A message sequence is one atomic bus transaction: repeated START between
 * messages, STOP only at end/error. No automatic write retry or bus recovery.
 * Backends return ENOTSUP for unsupported sequences, ETIMEDOUT on deadline,
 * ENODEV on address NACK and EIO on data NACK/arbitration/short transfer.
 * 10-bit capability is explicit; zero-length messages are not supported. */
#define NOS_I2C_READ 1u
#define NOS_I2C_TEN_BIT 2u
struct nos_i2c_message {
    void *data;
    size_t length;
    uint16_t address, flags;
};
struct nos_i2c_ops {
    int (*transfer)(void *, const struct nos_i2c_message *, size_t, uint32_t timeout_us);
    int (*recover)(void *, uint32_t timeout_us);
};
struct nos_i2c_bus {
    struct nos_controller controller;
    const struct nos_i2c_ops *ops;
    uint8_t ten_bit;
};
int nos_i2c_transfer(struct nos_i2c_bus *, const struct nos_i2c_message *, size_t, uint32_t);
int nos_i2c_recover(struct nos_i2c_bus *, uint32_t timeout_us);

/* UART read/write are NONBLOCKING: byte count (possibly partial) or negative
 * errno; nonempty operation with no progress returns EAGAIN, never zero. */
#define NOS_UART_READABLE 1u
#define NOS_UART_WRITABLE 2u
#define NOS_UART_ERROR 4u
struct nos_uart_config {
    uint32_t baud;
    uint8_t data_bits, stop_bits, parity, rts_cts;
};
struct nos_uart_ops {
    int (*configure)(void *, const struct nos_uart_config *);
    intptr_t (*read)(void *, void *, size_t);
    intptr_t (*write)(void *, const void *, size_t);
    int (*poll)(void *, uint32_t *events);
};
struct nos_uart {
    struct nos_controller controller;
    const struct nos_uart_ops *ops;
};
int nos_uart_configure(struct nos_uart *, const struct nos_uart_config *);
intptr_t nos_uart_read(struct nos_uart *, void *, size_t);
intptr_t nos_uart_write(struct nos_uart *, const void *, size_t);
int nos_uart_poll(struct nos_uart *, uint32_t *events);

/* USB device controller. Host: usb_host.h; dual role policy: usb_otg.h.
 * Class protocol and enumeration remain
 * above this boundary. send copies/consumes bytes, success means accepted, not
 * host-ACKed. Completion is delivered by the platform to its class event loop;
 * cancel must discard in-flight data and stale completion for this endpoint.
 * EP0 SETUP abort/reset lifecycle remains the class/platform responsibility.
 * Endpoint setup/address/remote wake are platform operations outside this v1
 * data-plane. Only enabled IN endpoints may be present in in_mask. */
struct nos_usb_device_ops {
    int (*send)(void *, uint8_t endpoint, const void *, size_t);
    int (*cancel)(void *, uint8_t endpoint);
    int (*halt)(void *, uint8_t endpoint, int enabled);
};
struct nos_usb_device_controller {
    struct nos_controller controller;
    const struct nos_usb_device_ops *ops;
    uint16_t in_mask;
    uint16_t max_packet;
};
int nos_usb_device_send(struct nos_usb_device_controller *, uint8_t, const void *, size_t);
int nos_usb_device_cancel(struct nos_usb_device_controller *, uint8_t);
int nos_usb_device_halt(struct nos_usb_device_controller *, uint8_t, int enabled);
#endif
