/* Copyright (C) 2026 NOS contributors. */
#include "driver/peripheral.h"

int nos_controller_begin(struct nos_controller *c)
{
    int error;
    if (!c || !c->device)
        return -NOS_DRV_EINVAL;
    if (c->busy)
        return -NOS_DRV_EBUSY;
    error = nos_device_get(c->device);
    if (!error)
        c->busy = 1;
    return error;
}
void nos_controller_end(struct nos_controller *c)
{
    if (c && c->busy) {
        c->busy = 0;
        nos_device_put(c->device);
    }
}
static int limit(const struct nos_controller *c, size_t bytes)
{
    return !c->max_transfer || c->max_transfer > NOS_DRIVER_MAX_TRANSFER || bytes > c->max_transfer
               ? -NOS_DRV_EINVAL
               : 0;
}
static int gpio_valid(const struct nos_gpio *p)
{
    return !p || !p->controller || !p->controller->ops || p->pin >= p->controller->pin_count ||
                   p->active_low > 1
               ? -NOS_DRV_EINVAL
               : 0;
}
int nos_gpio_configure(const struct nos_gpio *p, const struct nos_gpio_config *config)
{
    struct nos_gpio_config physical;
    int error = gpio_valid(p);
    if (error)
        return error;
    if (!config || config->direction > NOS_GPIO_OUTPUT || config->pull > NOS_GPIO_PULL_DOWN ||
        config->open_drain > 1 || config->initial_value > 1)
        return -NOS_DRV_EINVAL;
    if (!p->controller->ops->configure)
        return -NOS_DRV_ENOTSUP;
    physical = *config;
    physical.initial_value ^= p->active_low;
    error = nos_controller_begin(&p->controller->controller);
    if (error)
        return error;
    error = nos_driver_status(
        p->controller->ops->configure(p->controller->controller.context, p->pin, &physical));
    nos_controller_end(&p->controller->controller);
    return error;
}
int nos_gpio_read(const struct nos_gpio *p, int *value)
{
    int physical = 0, error = gpio_valid(p);
    if (error)
        return error;
    if (!value)
        return -NOS_DRV_EINVAL;
    if (!p->controller->ops->read)
        return -NOS_DRV_ENOTSUP;
    error = nos_controller_begin(&p->controller->controller);
    if (error)
        return error;
    error = nos_driver_status(
        p->controller->ops->read(p->controller->controller.context, p->pin, &physical));
    nos_controller_end(&p->controller->controller);
    if (!error && (physical < 0 || physical > 1))
        error = -NOS_DRV_EIO;
    if (!error)
        *value = physical ^ p->active_low;
    return error;
}
int nos_gpio_write(const struct nos_gpio *p, int value)
{
    int error = gpio_valid(p);
    if (error)
        return error;
    if (value < 0 || value > 1)
        return -NOS_DRV_EINVAL;
    if (!p->controller->ops->write)
        return -NOS_DRV_ENOTSUP;
    error = nos_controller_begin(&p->controller->controller);
    if (error)
        return error;
    error = nos_driver_status(p->controller->ops->write(p->controller->controller.context, p->pin,
                                                        value ^ p->active_low));
    nos_controller_end(&p->controller->controller);
    return error;
}
int nos_spi_transfer(const struct nos_spi_device *d, const struct nos_spi_segment *segments,
                     size_t count, uint32_t timeout_us)
{
    size_t i, total = 0;
    struct nos_spi_bus *b;
    int error;
    if (!d || !(b = d->bus) || !b->ops || !segments || !count || count > NOS_DRIVER_MAX_MESSAGES ||
        !timeout_us || timeout_us > INT32_MAX || !d->config.hz || d->config.hz > b->max_hz ||
        d->config.mode > 3 || !(b->mode_mask & (1u << d->config.mode)) || d->config.bits != 8 ||
        d->config.lsb_first > 1 || d->config.chip_select >= b->chip_select_count)
        return -NOS_DRV_EINVAL;
    for (i = 0; i < count; i++) {
        if (!segments[i].length || segments[i].length > NOS_DRIVER_MAX_TRANSFER - total)
            return -NOS_DRV_EINVAL;
        total += segments[i].length;
    }
    if ((error = limit(&b->controller, total)))
        return error;
    if (!b->ops->message)
        return -NOS_DRV_ENOTSUP;
    if ((error = nos_controller_begin(&b->controller)))
        return error;
    error = nos_driver_status(
        b->ops->message(b->controller.context, &d->config, segments, count, timeout_us));
    nos_controller_end(&b->controller);
    return error;
}
int nos_i2c_transfer(struct nos_i2c_bus *b, const struct nos_i2c_message *messages, size_t count,
                     uint32_t timeout_us)
{
    size_t i, total = 0;
    int error;
    if (!b || !b->ops || !messages || !count || count > NOS_DRIVER_MAX_MESSAGES || !timeout_us ||
        timeout_us > INT32_MAX)
        return -NOS_DRV_EINVAL;
    for (i = 0; i < count; i++) {
        const struct nos_i2c_message *m = &messages[i];
        if (!m->data || !m->length || m->length > NOS_DRIVER_MAX_TRANSFER - total ||
            m->flags & ~(NOS_I2C_READ | NOS_I2C_TEN_BIT) ||
            m->address > ((m->flags & NOS_I2C_TEN_BIT) ? 1023 : 127))
            return -NOS_DRV_EINVAL;
        if ((m->flags & NOS_I2C_TEN_BIT) && !b->ten_bit)
            return -NOS_DRV_ENOTSUP;
        total += m->length;
    }
    if ((error = limit(&b->controller, total)))
        return error;
    if (!b->ops->transfer)
        return -NOS_DRV_ENOTSUP;
    if ((error = nos_controller_begin(&b->controller)))
        return error;
    error = nos_driver_status(b->ops->transfer(b->controller.context, messages, count, timeout_us));
    nos_controller_end(&b->controller);
    return error;
}
int nos_i2c_recover(struct nos_i2c_bus *b, uint32_t timeout_us)
{
    int error;
    if (!b || !b->ops || !timeout_us || timeout_us > INT32_MAX)
        return -NOS_DRV_EINVAL;
    if (!b->ops->recover)
        return -NOS_DRV_ENOTSUP;
    if ((error = nos_controller_begin(&b->controller)))
        return error;
    error = nos_driver_status(b->ops->recover(b->controller.context, timeout_us));
    nos_controller_end(&b->controller);
    return error;
}
int nos_uart_configure(struct nos_uart *u, const struct nos_uart_config *config)
{
    int error;
    if (!u || !u->ops || !config || !config->baud || config->data_bits < 5 ||
        config->data_bits > 8 || config->stop_bits < 1 || config->stop_bits > 2 ||
        config->parity > 2 || config->rts_cts > 1)
        return -NOS_DRV_EINVAL;
    if (!u->ops->configure)
        return -NOS_DRV_ENOTSUP;
    if ((error = nos_controller_begin(&u->controller)))
        return error;
    error = nos_driver_status(u->ops->configure(u->controller.context, config));
    nos_controller_end(&u->controller);
    return error;
}
static intptr_t uart_io(struct nos_uart *u, void *rx, const void *tx, size_t count, int write)
{
    intptr_t result;
    int error;
    if (!u || !u->ops || (count && !(write ? tx : rx)) || limit(&u->controller, count))
        return -NOS_DRV_EINVAL;
    if (!(write ? !!u->ops->write : !!u->ops->read))
        return -NOS_DRV_ENOTSUP;
    if ((error = nos_controller_begin(&u->controller)))
        return error;
    if (!count)
        result = 0;
    else
        result = write ? u->ops->write(u->controller.context, tx, count)
                       : u->ops->read(u->controller.context, rx, count);
    nos_controller_end(&u->controller);
    if (result > 0 && (uintptr_t)result > count)
        return -NOS_DRV_EIO;
    return !result && count ? -NOS_DRV_EAGAIN : result;
}
intptr_t nos_uart_read(struct nos_uart *u, void *data, size_t count)
{
    return uart_io(u, data, NULL, count, 0);
}
intptr_t nos_uart_write(struct nos_uart *u, const void *data, size_t count)
{
    return uart_io(u, NULL, data, count, 1);
}
int nos_uart_poll(struct nos_uart *u, uint32_t *events)
{
    uint32_t ready = 0;
    int error;
    if (!u || !u->ops || !events)
        return -NOS_DRV_EINVAL;
    if (!u->ops->poll)
        return -NOS_DRV_ENOTSUP;
    if ((error = nos_controller_begin(&u->controller)))
        return error;
    error = nos_driver_status(u->ops->poll(u->controller.context, &ready));
    nos_controller_end(&u->controller);
    if (!error && (ready & ~(NOS_UART_READABLE | NOS_UART_WRITABLE | NOS_UART_ERROR)))
        error = -NOS_DRV_EIO;
    if (!error)
        *events = ready;
    return error;
}
static int usb_valid(struct nos_usb_device_controller *u, uint8_t ep)
{
    if (!u || !u->ops || (ep & 0x70) || !u->max_packet || u->max_packet > 1024)
        return -NOS_DRV_EINVAL;
    return 0;
}
int nos_usb_device_send(struct nos_usb_device_controller *u, uint8_t ep, const void *data,
                        size_t count)
{
    int error = usb_valid(u, ep);
    if (error)
        return error;
    if (!(ep & 0x80) || !(u->in_mask & (1u << (ep & 15))) || (count && !data) ||
        count > u->max_packet || limit(&u->controller, count))
        return -NOS_DRV_EINVAL;
    if (!u->ops->send)
        return -NOS_DRV_ENOTSUP;
    if ((error = nos_controller_begin(&u->controller)))
        return error;
    error = nos_driver_status(u->ops->send(u->controller.context, ep, data, count));
    nos_controller_end(&u->controller);
    return error;
}
int nos_usb_device_cancel(struct nos_usb_device_controller *u, uint8_t ep)
{
    int error = usb_valid(u, ep);
    if (error)
        return error;
    if (!u->ops->cancel)
        return -NOS_DRV_ENOTSUP;
    if ((error = nos_controller_begin(&u->controller)))
        return error;
    error = nos_driver_status(u->ops->cancel(u->controller.context, ep));
    nos_controller_end(&u->controller);
    return error;
}
int nos_usb_device_halt(struct nos_usb_device_controller *u, uint8_t ep, int enabled)
{
    int error = usb_valid(u, ep);
    if (error)
        return error;
    if (enabled < 0 || enabled > 1)
        return -NOS_DRV_EINVAL;
    if (!u->ops->halt)
        return -NOS_DRV_ENOTSUP;
    if ((error = nos_controller_begin(&u->controller)))
        return error;
    error = nos_driver_status(u->ops->halt(u->controller.context, ep, enabled));
    nos_controller_end(&u->controller);
    return error;
}
