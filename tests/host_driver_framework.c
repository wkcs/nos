/* Copyright (C) 2026 NOS contributors. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/peripheral.h"
#include "driver/spi_nor.h"
#include "driver/uart_16550.h"
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                                        \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static int probe_error, pm_error, acquired, released, removed, calls, callback_status;
static uint64_t release_order[16];
static struct nos_resource_lease leases[2];
static int acquire(void *ctx, const struct nos_resource *r, uint64_t *token)
{
    (void)ctx;
    *token = r->id + 100u;
    acquired++;
    return 0;
}
static void release(void *ctx, const struct nos_resource *r, uint64_t token)
{
    (void)ctx;
    CHECK(token == r->id + 100u);
    release_order[released++] = token;
}
static const struct nos_resource_provider provider = {NULL, acquire, release};
static int probe(struct nos_device *d)
{
    if (d->resource_count) {
        CHECK(nos_device_acquire_resource(d, NOS_RESOURCE_MMIO, 0, &provider, &leases[0]) == 0);
        CHECK(nos_device_acquire_resource(d, NOS_RESOURCE_MMIO, 0, &provider, &leases[1]) ==
              -NOS_DRV_EBUSY);
        CHECK(nos_device_acquire_resource(d, NOS_RESOURCE_CLOCK, 1, &provider, &leases[1]) == 0);
    }
    return probe_error;
}
static void remove_device(struct nos_device *d)
{
    CHECK(d->state == NOS_DEVICE_PROBING);
    if (d->resource_count)
        CHECK(d->lease_count == 2);
    removed++;
}
static int pm(struct nos_device *d)
{
    CHECK(d->state == NOS_DEVICE_PROBING);
    return pm_error;
}
static const struct nos_driver driver = {"test,controller", probe, remove_device, pm, pm};
static void lifecycle(void)
{
    struct nos_device *entries[3];
    struct nos_device_registry registry = {entries, 3, 0};
    const struct nos_resource resources[] = {{0x1000, 0x100, 0, NOS_RESOURCE_MMIO},
                                             {0, 0, 1, NOS_RESOURCE_CLOCK}};
    struct nos_device parent = {.name = "parent", .compatible = "test,controller"};
    struct nos_device child = {.name = "child",
                               .compatible = "test,controller",
                               .parent = &parent,
                               .resources = resources,
                               .resource_count = 2};
    struct nos_device duplicate = {.name = "child", .compatible = "test,controller"};
    CHECK(nos_device_register(&registry, &child) == -NOS_DRV_ENODEV);
    CHECK(nos_device_register(&registry, &parent) == 0);
    CHECK(nos_device_register(&registry, &child) == 0);
    CHECK(nos_device_register(&registry, &duplicate) == -NOS_DRV_EBUSY);
    CHECK(nos_device_find(&registry, "child") == &child);
    CHECK(!nos_device_find(&registry, "missing"));
    CHECK(nos_device_bind(&child, &driver) == -NOS_DRV_ENODEV);
    CHECK(nos_device_bind(&parent, &driver) == 0);
    probe_error = -NOS_DRV_EIO;
    CHECK(nos_device_bind(&child, &driver) == probe_error);
    CHECK(child.state == NOS_DEVICE_FAILED && !child.driver && !child.leases && !parent.users);
    CHECK(acquired == 2 && released == 2 && release_order[0] == 101 && release_order[1] == 100);
    CHECK(!leases[0].owner && !leases[1].owner);
    probe_error = 1;
    CHECK(nos_device_bind(&child, &driver) == -NOS_DRV_EIO);
    probe_error = 0;
    CHECK(nos_device_bind(&child, &driver) == 0 && parent.users == 1);
    CHECK(nos_device_unregister(&registry, &child) == -NOS_DRV_EBUSY);
    CHECK(nos_device_unbind(&parent) == -NOS_DRV_EBUSY);
    CHECK(nos_device_suspend(&parent) == -NOS_DRV_EBUSY);
    CHECK(nos_device_get(&child) == 0);
    CHECK(nos_device_suspend(&child) == -NOS_DRV_EBUSY);
    CHECK(nos_device_unbind(&child) == -NOS_DRV_EBUSY);
    nos_device_put(&child);
    pm_error = -NOS_DRV_EIO;
    CHECK(nos_device_suspend(&child) == pm_error && child.state == NOS_DEVICE_ACTIVE);
    pm_error = 0;
    CHECK(nos_device_suspend(&child) == 0);
    CHECK(nos_device_get(&child) == -NOS_DRV_ENODEV);
    pm_error = 1;
    CHECK(nos_device_resume(&child) == -NOS_DRV_EIO && child.state == NOS_DEVICE_SUSPENDED);
    pm_error = 0;
    CHECK(nos_device_resume(&child) == 0);
    child.users = UINT16_MAX;
    CHECK(nos_device_get(&child) == -NOS_DRV_EBUSY);
    child.users = 0;
    CHECK(nos_device_unbind(&child) == 0 && !parent.users);
    CHECK(nos_device_unregister(&registry, &parent) == -NOS_DRV_EBUSY);
    CHECK(nos_device_unregister(&registry, &child) == 0);
    CHECK(nos_device_unbind(&parent) == 0);
    CHECK(nos_device_unregister(&registry, &parent) == 0 && registry.count == 0);
    CHECK(acquired == released && removed == 4);
    {
        struct nos_resource invalid[] = {{UINT64_MAX, 2, 0, NOS_RESOURCE_MMIO}};
        struct nos_device bad = {.name = "bad",
                                 .compatible = "test,controller",
                                 .resources = invalid,
                                 .resource_count = 1};
        CHECK(nos_device_register(&registry, &bad) == -NOS_DRV_EINVAL);
        invalid[0].start = 0;
        invalid[0].size = 0;
        CHECK(nos_device_register(&registry, &bad) == -NOS_DRV_EINVAL);
        registry.capacity = 0;
        CHECK(nos_device_register(&registry, &parent) == -NOS_DRV_ENOSPC);
    }
    puts("PASS driver_lifecycle_parent_leases_reverse_rollback_pm_errors_and_bounds");
}
static int gpio_value, gpio_bad;
static int gpio_config(void *ctx, uint32_t pin, const struct nos_gpio_config *config)
{
    (void)ctx;
    CHECK(pin == 3);
    calls++;
    gpio_value = config->initial_value;
    return callback_status;
}
static int gpio_read(void *ctx, uint32_t pin, int *value)
{
    (void)ctx;
    CHECK(pin == 3);
    calls++;
    *value = gpio_bad ? 3 : gpio_value;
    return callback_status;
}
static int gpio_write(void *ctx, uint32_t pin, int value)
{
    (void)ctx;
    CHECK(pin == 3);
    calls++;
    gpio_value = value;
    return callback_status;
}
static const struct nos_gpio_ops gpio_ops = {gpio_config, gpio_read, gpio_write};
static struct nos_spi_bus *current_spi;
static int spi(void *ctx, const struct nos_spi_config *config, const struct nos_spi_segment *s,
               size_t count, uint32_t timeout)
{
    (void)ctx;
    CHECK(config->hz == 1000000 && count == 2 && timeout == 500);
    CHECK(current_spi->controller.busy && current_spi->controller.device->users == 1);
    struct nos_spi_device again = {current_spi, *config};
    CHECK(nos_spi_transfer(&again, s, count, timeout) == -NOS_DRV_EBUSY);
    CHECK(nos_device_unbind(current_spi->controller.device) == -NOS_DRV_EBUSY);
    CHECK(s[0].length == 2 && s[1].length == 3 && s[1].tx_fill == 0xa5);
    memset(s[1].rx, s[1].tx_fill, s[1].length);
    calls++;
    return callback_status;
}
static const struct nos_spi_ops spi_ops = {spi};
static int i2c(void *ctx, const struct nos_i2c_message *messages, size_t count, uint32_t timeout)
{
    (void)ctx;
    CHECK(count == 2 && timeout == 500);
    CHECK(messages[0].address == 0x50 && !messages[0].flags);
    CHECK(messages[1].address == 0x50 && messages[1].flags == NOS_I2C_READ);
    memset(messages[1].data, 0x42, messages[1].length);
    calls++;
    return callback_status;
}
static int recover(void *ctx, uint32_t timeout)
{
    (void)ctx;
    CHECK(timeout == 500);
    calls++;
    return callback_status;
}
static const struct nos_i2c_ops i2c_ops = {i2c, recover};
static intptr_t uart_result;
static int uart_config(void *ctx, const struct nos_uart_config *config)
{
    (void)ctx;
    CHECK(config->baud == 115200);
    calls++;
    return callback_status;
}
static intptr_t uart_read(void *ctx, void *data, size_t n)
{
    (void)ctx;
    if (uart_result > 0 && (size_t)uart_result <= n)
        memset(data, 'X', (size_t)uart_result);
    calls++;
    return uart_result;
}
static intptr_t uart_write(void *ctx, const void *data, size_t n)
{
    (void)ctx;
    CHECK(data && n);
    calls++;
    return uart_result;
}
static int uart_poll(void *ctx, uint32_t *events)
{
    (void)ctx;
    *events = NOS_UART_READABLE | NOS_UART_WRITABLE;
    calls++;
    return callback_status;
}
static const struct nos_uart_ops uart_ops = {uart_config, uart_read, uart_write, uart_poll};
static int usb_send(void *ctx, uint8_t ep, const void *data, size_t count)
{
    (void)ctx;
    CHECK(ep == 0x81 && (!count || data));
    calls++;
    return callback_status;
}
static int usb_cancel(void *ctx, uint8_t ep)
{
    (void)ctx;
    CHECK(ep == 0x81);
    calls++;
    return callback_status;
}
static int usb_halt(void *ctx, uint8_t ep, int enable)
{
    (void)ctx;
    CHECK(ep == 0x81 && enable == 1);
    calls++;
    return callback_status;
}
static const struct nos_usb_device_ops usb_ops = {usb_send, usb_cancel, usb_halt};
static void peripherals(void)
{
    struct nos_device *entries[1];
    struct nos_device_registry registry = {entries, 1, 0};
    struct nos_device device = {.name = "controller", .compatible = "test,controller"};
    struct nos_gpio_controller gc = {{&device, NULL, 1, 0}, &gpio_ops, 16};
    struct nos_gpio pin = {&gc, 3, 1};
    struct nos_gpio_config config = {NOS_GPIO_OUTPUT, NOS_GPIO_PULL_UP, 0, 1};
    struct nos_spi_bus sb = {{&device, NULL, 8, 0}, &spi_ops, 1000000, 1, 15};
    struct nos_spi_device sd = {&sb, {1000000, 0, 3, 8, 0}};
    uint8_t address[2] = {1, 2}, data[3] = {0};
    struct nos_spi_segment ss[] = {{address, NULL, 2, 0}, {NULL, data, 3, 0xa5}};
    struct nos_i2c_bus ib = {{&device, NULL, 8, 0}, &i2c_ops, 0};
    struct nos_i2c_message im[] = {{address, 2, 0x50, 0}, {data, 3, 0x50, NOS_I2C_READ}};
    struct nos_uart uart = {{&device, NULL, 8, 0}, &uart_ops};
    struct nos_uart_config uc = {115200, 8, 1, 0, 0};
    struct nos_usb_device_controller usb = {{&device, NULL, 64, 0}, &usb_ops, 3, 64};
    int value = -1, before;
    uint32_t events;
    CHECK(nos_gpio_read(&pin, &value) == -NOS_DRV_ENODEV && value == -1);
    CHECK(nos_device_register(&registry, &device) == 0 && nos_device_bind(&device, &driver) == 0);
    CHECK(nos_gpio_configure(&pin, &config) == 0 && gpio_value == 0);
    CHECK(nos_gpio_read(&pin, &value) == 0 && value == 1);
    CHECK(nos_gpio_write(&pin, 0) == 0 && gpio_value == 1);
    gpio_bad = 1;
    CHECK(nos_gpio_read(&pin, &value) == -NOS_DRV_EIO && value == 1);
    gpio_bad = 0;
    CHECK(nos_gpio_write(&pin, 2) == -NOS_DRV_EINVAL);
    pin.pin = 16;
    before = calls;
    CHECK(nos_gpio_write(&pin, 1) == -NOS_DRV_EINVAL && calls == before);
    pin.pin = 3;
    current_spi = &sb;
    CHECK(nos_spi_transfer(&sd, ss, 2, 500) == 0 && data[0] == 0xa5 && !device.users &&
          !sb.controller.busy);
    callback_status = -NOS_DRV_ETIMEDOUT;
    CHECK(nos_spi_transfer(&sd, ss, 2, 500) == callback_status && !device.users &&
          !sb.controller.busy);
    callback_status = 1;
    CHECK(nos_spi_transfer(&sd, ss, 2, 500) == -NOS_DRV_EIO);
    callback_status = 0;
    before = calls;
    ss[1].length = SIZE_MAX;
    CHECK(nos_spi_transfer(&sd, ss, 2, 500) == -NOS_DRV_EINVAL && calls == before);
    ss[1].length = 3;
    CHECK(nos_spi_transfer(&sd, ss, 2, 0) == -NOS_DRV_EINVAL);
    sd.config.mode = 4;
    CHECK(nos_spi_transfer(&sd, ss, 2, 500) == -NOS_DRV_EINVAL);
    sd.config.mode = 3;
    CHECK(nos_i2c_transfer(&ib, im, 2, 500) == 0 && data[0] == 0x42);
    im[1].flags = NOS_I2C_READ | NOS_I2C_TEN_BIT;
    CHECK(nos_i2c_transfer(&ib, im, 2, 500) == -NOS_DRV_ENOTSUP);
    im[1].flags = NOS_I2C_READ;
    im[1].address = 128;
    CHECK(nos_i2c_transfer(&ib, im, 2, 500) == -NOS_DRV_EINVAL);
    im[1].address = 0x50;
    im[1].length = SIZE_MAX;
    CHECK(nos_i2c_transfer(&ib, im, 2, 500) == -NOS_DRV_EINVAL);
    im[1].length = 3;
    callback_status = -NOS_DRV_ETIMEDOUT;
    CHECK(nos_i2c_transfer(&ib, im, 2, 500) == callback_status && !ib.controller.busy);
    callback_status = 0;
    CHECK(nos_i2c_recover(&ib, 500) == 0);
    CHECK(nos_uart_configure(&uart, &uc) == 0);
    uc.parity = 3;
    CHECK(nos_uart_configure(&uart, &uc) == -NOS_DRV_EINVAL);
    uart_result = 2;
    CHECK(nos_uart_read(&uart, data, 3) == 2 && data[0] == 'X');
    CHECK(nos_uart_write(&uart, data, 3) == 2);
    uart_result = 4;
    CHECK(nos_uart_read(&uart, data, 3) == -NOS_DRV_EIO);
    uart_result = 0;
    CHECK(nos_uart_read(&uart, data, 3) == -NOS_DRV_EAGAIN);
    CHECK(nos_uart_read(&uart, NULL, 0) == 0);
    uart_result = -NOS_DRV_EIO;
    CHECK(nos_uart_write(&uart, data, 3) == uart_result);
    CHECK(nos_uart_poll(&uart, &events) == 0 && events == 3);
    CHECK(nos_usb_device_send(&usb, 0x81, data, 3) == 0);
    CHECK(nos_usb_device_send(&usb, 0x81, NULL, 0) == 0);
    CHECK(nos_usb_device_send(&usb, 0x81, data, 65) == -NOS_DRV_EINVAL);
    CHECK(nos_usb_device_send(&usb, 0x82, data, 3) == -NOS_DRV_EINVAL);
    CHECK(nos_usb_device_send(&usb, 0x01, data, 3) == -NOS_DRV_EINVAL);
    CHECK(nos_usb_device_cancel(&usb, 0x81) == 0 && nos_usb_device_halt(&usb, 0x81, 1) == 0);
    callback_status = -NOS_DRV_EAGAIN;
    CHECK(nos_usb_device_send(&usb, 0x81, data, 3) == callback_status && !usb.controller.busy);
    callback_status = 0;
    CHECK(nos_device_suspend(&device) == 0);
    CHECK(nos_spi_transfer(&sd, ss, 2, 500) == -NOS_DRV_ENODEV);
    CHECK(nos_uart_read(&uart, data, 1) == -NOS_DRV_ENODEV);
    CHECK(nos_usb_device_send(&usb, 0x81, data, 1) == -NOS_DRV_ENODEV);
    CHECK(nos_device_unbind(&device) == 0);
    puts(
        "PASS "
        "driver_gpio_polarity_spi_atomic_segments_reentry_i2c_restart_uart_short_usb_backpressure");
}
/* Independent SPI controller model: no NK60 headers/register layout. The same
 * generic W25Q driver operates through this entire-message controller. */
static uint8_t memory[1024 * 1024];
static int wel, protected_flash, flash_time_stalled, nor_busy;
static uint32_t stamp;
static unsigned nor_messages;
static int nor_message(void *ctx, const struct nos_spi_config *config,
                       const struct nos_spi_segment *segments, size_t count, uint32_t timeout)
{
    const uint8_t *h = segments[0].tx;
    uint32_t offset = 0;
    size_t i;
    (void)ctx;
    CHECK(config->mode == 0 && timeout == 20000);
    nor_messages++;
    if (segments[0].length == 4)
        offset = ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
    switch (h[0]) {
    case 0xab:
        CHECK(count == 1);
        break;
    case 0x9f:
        CHECK(count == 2 && segments[1].length == 3);
        memcpy(segments[1].rx, "\xef\x40\x14", 3);
        break;
    case 0x05:
        CHECK(count == 2);
        *(uint8_t *)segments[1].rx = (uint8_t)((wel ? 2 : 0) | (nor_busy ? 1 : 0));
        break;
    case 0x06:
        wel = 1;
        break;
    case 0x03:
        CHECK(count == 2);
        memcpy(segments[1].rx, memory + offset, segments[1].length);
        break;
    case 0x02:
        CHECK(wel && count == 2 &&
              offset / 256 == (offset + (uint32_t)segments[1].length - 1) / 256);
        if (!protected_flash)
            for (i = 0; i < segments[1].length; i++)
                memory[offset + i] &= ((const uint8_t *)segments[1].tx)[i];
        wel = 0;
        break;
    case 0x20:
        CHECK(wel && !(offset % 4096));
        memset(memory + offset, 0xff, 4096);
        wel = 0;
        break;
    default:
        CHECK(0);
    }
    return 0;
}
static const struct nos_spi_ops nor_ops = {nor_message};
static uint32_t now(void *ctx)
{
    (void)ctx;
    if (!flash_time_stalled)
        stamp += 10;
    return stamp;
}
static void delay(void *ctx, uint32_t us)
{
    (void)ctx;
    if (!flash_time_stalled)
        stamp += us;
}
static void portable_nor(void)
{
    struct nos_device *entries[1];
    struct nos_device_registry registry = {entries, 1, 0};
    struct nos_device device = {.name = "unrelated-spi", .compatible = "test,controller"};
    struct nos_spi_bus bus = {{&device, NULL, 260, 0}, &nor_ops, 4000000, 1, 1};
    struct nos_spi_nor n = {
        .spi = {&bus, {4000000, 0, 0, 8, 0}}, .time_us = now, .delay_us = delay};
    uint8_t bytes[600], actual[600];
    uint32_t id;
    memset(memory, 0xff, sizeof(memory));
    memset(bytes, 0x5a, sizeof(bytes));
    CHECK(nos_device_register(&registry, &device) == 0 && nos_device_bind(&device, &driver) == 0);
    CHECK(nos_spi_nor_probe(&n, &id) == 0 && id == 0xef4014 && n.capacity == sizeof(memory));
    CHECK(nos_spi_nor_program(&n, 250, bytes, sizeof(bytes)) == 0);
    CHECK(nos_spi_nor_read(&n, 250, actual, sizeof(actual)) == 0 &&
          !memcmp(actual, bytes, sizeof(bytes)));
    CHECK(nos_spi_nor_read(&n, UINT32_MAX, actual, 1) == -NOS_DRV_EINVAL);
    CHECK(nos_spi_nor_erase(&n, 0) == 0 && memory[250] == 0xff);
    protected_flash = 1;
    CHECK(nos_spi_nor_program(&n, 0, bytes, 1) == -NOS_DRV_EIO);
    nor_busy = 1;
    flash_time_stalled = 1;
    nor_messages = 0;
    CHECK(nos_spi_nor_read(&n, 0, actual, 1) == -NOS_DRV_ETIMEDOUT && nor_messages == 100000);
    CHECK(!device.users && !bus.controller.busy);
    puts("PASS driver_portable_nor_independent_controller_page_verify_protect_stalled_clock");
}
static uint32_t uart_regs[8];
static unsigned uart_writes;
static uint32_t reg_read(void *ctx, unsigned index)
{
    (void)ctx;
    if (index == 0 && !(uart_regs[3] & 128))
        uart_regs[5] &= ~1u;
    return uart_regs[index];
}
static void reg_write(void *ctx, unsigned index, uint32_t value)
{
    (void)ctx;
    uart_regs[index] = value;
    if (index == 0 && !(uart_regs[3] & 128)) {
        uart_writes++;
        uart_regs[5] &= ~0x60u;
    }
}
static void uart_16550(void)
{
    struct nos_device *entries[1];
    struct nos_device_registry registry = {entries, 1, 0};
    struct nos_device d = {.name = "serial", .compatible = "test,controller"};
    struct nos_uart_16550 p = {.read = reg_read, .write = reg_write, .clock_hz = 1843200};
    struct nos_uart_config c = {115200, 8, 1, 0, 0};
    uint8_t data[3] = {1, 2, 3};
    CHECK(nos_device_register(&registry, &d) == 0 && nos_device_bind(&d, &driver) == 0);
    CHECK(nos_uart_16550_init(&p, &d) == 0);
    uart_regs[5] = 0x60;
    CHECK(nos_uart_configure(&p.uart, &c) == 0 && uart_regs[0] == 1 && uart_regs[1] == 0 &&
          uart_regs[3] == 3);
    CHECK(nos_uart_write(&p.uart, data, 3) == 1 && uart_writes == 1);
    CHECK(nos_uart_write(&p.uart, data, 3) == -NOS_DRV_EAGAIN);
    CHECK(nos_uart_configure(&p.uart, &c) == -NOS_DRV_EBUSY);
    uart_regs[5] = 0x61;
    uart_regs[0] = 'A';
    CHECK(nos_uart_read(&p.uart, data, 3) == 1 && data[0] == 'A');
    CHECK(nos_uart_read(&p.uart, data, 3) == -NOS_DRV_EAGAIN);
    c.baud = 123456;
    CHECK(nos_uart_configure(&p.uart, &c) == -NOS_DRV_ENOTSUP);
    c.baud = 115200;
    c.rts_cts = 1;
    CHECK(nos_uart_configure(&p.uart, &c) == -NOS_DRV_ENOTSUP);
    c.rts_cts = 0;
    c.data_bits = 5;
    c.stop_bits = 2;
    CHECK(nos_uart_configure(&p.uart, &c) == -NOS_DRV_ENOTSUP);
    uart_regs[5] = 0x63;
    CHECK(nos_uart_read(&p.uart, data, 3) == -NOS_DRV_EIO);
    CHECK(!d.users && !p.uart.controller.busy);
    puts("PASS driver_16550_real_register_driver_divisor_short_io_errors_no_spin");
}
int main(void)
{
    lifecycle();
    peripherals();
    portable_nor();
    uart_16550();
    return 0;
}
