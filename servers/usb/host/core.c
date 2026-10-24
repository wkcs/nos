/* Copyright (C) 2026 NOS contributors. */
#include "driver/usb_host.h"
static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}
static void invalidate(struct nos_usb_host *h)
{
    h->configured = 0;
    h->endpoint_count = h->interface_count = 0;
}
static int valid(struct nos_usb_host *h)
{
    return h && h->ops && h->ops->start && h->ops->stop && h->ops->port_status &&
           h->ops->port_reset && h->ops->packet && h->ops->time_us && h->ops->delay_us;
}
static int begin(struct nos_usb_host *h)
{
    if (!valid(h))
        return -NOS_DRV_EINVAL;
    if (!h->running)
        return -NOS_DRV_ENODEV;
    if (h->faulted)
        return -NOS_DRV_EIO;
    return nos_controller_begin(&h->controller);
}
int nos_usb_host_start(struct nos_usb_host *h)
{
    int error, retain = 0;
    if (!valid(h) || !h->controller.max_transfer ||
        h->controller.max_transfer > NOS_DRIVER_MAX_TRANSFER)
        return -NOS_DRV_EINVAL;
    if (h->running)
        return -NOS_DRV_EBUSY;
    if ((error = nos_controller_begin(&h->controller)))
        return error;
    error = nos_driver_status(h->ops->start(h->controller.context));
    if (!error) {
        h->running = 1;
        h->faulted = 0;
        invalidate(h);
        retain = 1;
    } else if (nos_driver_status(h->ops->stop(h->controller.context))) {
        /* Partial start may own DMA. Keep the reference already obtained by
         * begin(), including when the 16-bit reference count is saturated. */
        h->running = h->faulted = 1;
        retain = 1;
    }
    if (retain)
        h->controller.busy = 0; /* This reference now belongs to the host session. */
    else
        nos_controller_end(&h->controller);
    return error;
}
int nos_usb_host_stop(struct nos_usb_host *h)
{
    int error;
    if (!valid(h))
        return -NOS_DRV_EINVAL;
    if (!h->running)
        return 0;
    if ((error = nos_controller_begin(&h->controller)))
        return error;
    error = nos_driver_status(h->ops->stop(h->controller.context));
    invalidate(h);
    if (!error) {
        h->running = h->faulted = 0;
        nos_device_put(h->controller.device);
    } else
        h->faulted = 1;
    nos_controller_end(&h->controller);
    return error;
}
static int port(struct nos_usb_host *h, uint32_t *flags, int check_change)
{
    uint32_t status = 0;
    int error = nos_driver_status(h->ops->port_status(h->controller.context, &status));
    if (error)
        return error;
    if (status & ~(NOS_USB_PORT_CONNECTED | NOS_USB_PORT_CHANGED | NOS_USB_PORT_OVERCURRENT |
                   NOS_USB_PORT_ENABLED))
        return -NOS_DRV_EIO;
    *flags = status;
    if (!(status & NOS_USB_PORT_CONNECTED) || (status & NOS_USB_PORT_OVERCURRENT) ||
        (check_change && (status & NOS_USB_PORT_CHANGED))) {
        invalidate(h);
        return -NOS_DRV_ENODEV;
    }
    return 0;
}
int nos_usb_host_poll(struct nos_usb_host *h, uint32_t *flags)
{
    uint32_t status = 0;
    int error;
    if (!flags)
        return -NOS_DRV_EINVAL;
    if ((error = begin(h)))
        return error;
    error = port(h, &status, 1);
    *flags = status;
    nos_controller_end(&h->controller);
    return error;
}
static uint32_t remaining(struct nos_usb_host *h, uint32_t start, uint32_t timeout)
{
    uint32_t elapsed = h->ops->time_us(h->controller.context) - start;
    return elapsed >= timeout ? 0 : timeout - elapsed;
}
static int packet(struct nos_usb_host *h, struct nos_usb_packet *p, size_t *actual, uint32_t start,
                  uint32_t timeout, int retry)
{
    unsigned loops;
    *actual = 0;
    for (loops = 0; loops < 100000u; loops++) {
        uint32_t flags, left = remaining(h, start, timeout);
        int error;
        size_t count = 0;
        if (!left)
            return -NOS_DRV_ETIMEDOUT;
        error = port(h, &flags, 1);
        if (error)
            return error;
        error = nos_driver_status(h->ops->packet(h->controller.context, p, &count, left));
        if (!error) {
            if (count > p->length || (!(p->endpoint & 0x80) && count != p->length))
                return -NOS_DRV_EIO;
            *actual = count;
            return 0;
        }
        if (error != -NOS_DRV_EAGAIN || !retry)
            return error;
        h->ops->delay_us(h->controller.context, left < 100 ? left : 100);
    }
    return -NOS_DRV_ETIMEDOUT;
}
static int control(struct nos_usb_host *h, const uint8_t setup[8], void *data, size_t length,
                   size_t *actual, uint32_t timeout)
{
    struct nos_usb_packet p = {
        (void *)setup, 8, h->ep0_size, h->address, 0, h->speed, NOS_USB_CONTROL, NOS_USB_SETUP};
    uint32_t start = h->ops->time_us(h->controller.context);
    size_t count, done = 0;
    uint8_t toggle = NOS_USB_DATA1;
    int error = packet(h, &p, &count, start, timeout, 1);
    *actual = 0;
    if (error)
        return error;
    while (done < length) {
        size_t part = length - done;
        if (part > h->ep0_size)
            part = h->ep0_size;
        p.data = (uint8_t *)data + done;
        p.length = part;
        p.endpoint = setup[0] & 0x80;
        p.pid = toggle;
        error = packet(h, &p, &count, start, timeout, 1);
        if (error) {
            *actual = done;
            return error;
        }
        done += count;
        toggle ^= 1;
        if (count < part)
            break;
    }
    *actual = done;
    p.data = NULL;
    p.length = 0;
    p.endpoint = length && (setup[0] & 0x80) ? 0 : 0x80;
    p.pid = NOS_USB_DATA1;
    return packet(h, &p, &count, start, timeout, 1);
}
static int request(struct nos_usb_host *h, uint8_t type, uint8_t req, uint16_t value,
                   uint16_t index, void *data, uint16_t length, size_t *actual)
{
    uint8_t setup[8] = {type,
                        req,
                        (uint8_t)value,
                        (uint8_t)(value >> 8),
                        (uint8_t)index,
                        (uint8_t)(index >> 8),
                        (uint8_t)length,
                        (uint8_t)(length >> 8)};
    return control(h, setup, data, length, actual, 500000);
}
static int endpoint_size(uint8_t speed, uint8_t type, uint16_t size, uint8_t interval)
{
    if (!size || size & 0xf800u || type == NOS_USB_ISOCHRONOUS)
        return 0;
    if (speed == NOS_USB_LOW)
        return type == NOS_USB_INTERRUPT && size <= 8 && interval >= 10;
    if (speed == NOS_USB_FULL) {
        if (type == NOS_USB_BULK)
            return size == 8 || size == 16 || size == 32 || size == 64;
        return type == NOS_USB_INTERRUPT && size <= 64 && interval;
    }
    if (speed == NOS_USB_HIGH) {
        if (type == NOS_USB_BULK)
            return size == 512;
        return type == NOS_USB_INTERRUPT && size <= 1024 && interval && interval <= 16;
    }
    return 0;
}
/* Validate in a first pass, then populate. No temporary 512-byte config copy. */
static int parse(struct nos_usb_host *h, const uint8_t *d, size_t length, int commit)
{
    size_t offset = 9;
    uint8_t interfaces = 0, endpoints = 0, current = 0, alternate = 0, expected = 0, seen = 0;
    uint8_t numbers[NOS_USB_INTERFACES], used[32] = {0}, have_interface = 0;
    unsigned i;
    if (!d || length < 9 || length > NOS_USB_CONFIG_MAX || d[0] != 9 || d[1] != 2 ||
        le16(d + 2) != length || !d[4] || d[4] > NOS_USB_INTERFACES || !d[5] || !(d[7] & 0x80) ||
        (d[7] & 0x1f))
        return -NOS_DRV_EINVAL;
    if ((uint16_t)d[8] * 2u > h->power_budget_ma)
        return -NOS_DRV_ENOSPC;
    while (offset < length) {
        const uint8_t *p = d + offset;
        size_t size = p[0];
        if (length - offset < 2 || size < 2 || size > length - offset)
            return -NOS_DRV_EINVAL;
        if (p[1] == 4) {
            if (size != 9 || (have_interface && seen != expected))
                return -NOS_DRV_EINVAL;
            have_interface = 1;
            current = p[2];
            alternate = p[3];
            expected = p[4];
            seen = 0;
            if (p[5] == 9)
                return -NOS_DRV_ENOTSUP;
            if (alternate) {
                for (i = 0; i < interfaces && numbers[i] != current; i++)
                    ;
                if (i == interfaces)
                    return -NOS_DRV_EINVAL;
            }
            if (!alternate) {
                if (interfaces == NOS_USB_INTERFACES)
                    return -NOS_DRV_ENOSPC;
                for (i = 0; i < interfaces; i++)
                    if (numbers[i] == current)
                        return -NOS_DRV_EINVAL;
                numbers[interfaces] = current;
                if (commit)
                    h->interfaces[interfaces] =
                        (struct nos_usb_interface){current, p[5], p[6], p[7]};
                interfaces++;
            }
        } else if (p[1] == 5) {
            uint8_t address, type;
            uint16_t packet_size;
            if (size < 7 || !have_interface || seen == 255)
                return -NOS_DRV_EINVAL;
            seen++;
            address = p[2];
            type = p[3] & 3;
            packet_size = le16(p + 4);
            if (!(address & 15) || (address & 0x70) || (p[3] & 0xfc))
                return -NOS_DRV_EINVAL;
            if (alternate) {
                offset += size;
                continue;
            }
            if (type == NOS_USB_CONTROL || type == NOS_USB_ISOCHRONOUS)
                return -NOS_DRV_ENOTSUP;
            if (!endpoint_size(h->speed, type, packet_size, p[6]))
                return -NOS_DRV_EINVAL;
            i = (address & 15) + ((address & 0x80) ? 16 : 0);
            if (used[i])
                return -NOS_DRV_EINVAL;
            used[i] = 1;
            if (endpoints == NOS_USB_ENDPOINTS)
                return -NOS_DRV_ENOSPC;
            if (commit)
                h->endpoints[endpoints] = (struct nos_usb_endpoint){.max_packet = packet_size,
                                                                    .address = address,
                                                                    .type = type,
                                                                    .interval = p[6],
                                                                    .interface_number = current};
            endpoints++;
        } else if (p[1] == 2 || p[1] == 1)
            return -NOS_DRV_EINVAL;
        offset += size;
    }
    if ((have_interface && seen != expected) || interfaces != d[4])
        return -NOS_DRV_EINVAL;
    if (commit) {
        h->endpoint_count = endpoints;
        h->interface_count = interfaces;
        h->config_length = (uint16_t)length;
        h->configuration_value = d[5];
    }
    return 0;
}
int nos_usb_host_parse_configuration(struct nos_usb_host *h, const uint8_t *d, size_t length)
{
    int error;
    if (!h || h->configured)
        return -NOS_DRV_EINVAL;
    error = parse(h, d, length, 0);
    return error ? error : parse(h, d, length, 1);
}
int nos_usb_host_enumerate(struct nos_usb_host *h, uint32_t *handle)
{
    uint32_t flags;
    uint8_t descriptor[18], config[9];
    size_t count = 0;
    int error;
    if (!handle)
        return -NOS_DRV_EINVAL;
    *handle = 0;
    if ((error = begin(h)))
        return error;
    invalidate(h);
    if (h->generation == UINT32_MAX) {
        error = -NOS_DRV_ENOSPC;
        goto out;
    }
    h->generation++;
    error = port(h, &flags, 0);
    if (error)
        goto out;
    h->ops->delay_us(h->controller.context, 100000); /* attach debounce */
    error = nos_driver_status(h->ops->port_reset(h->controller.context, &h->speed));
    if (error)
        goto out;
    if (h->speed < NOS_USB_LOW || h->speed > NOS_USB_HIGH) {
        error = -NOS_DRV_ENOTSUP;
        goto out;
    }
    /* Consume reset/connection status before default-address requests. */
    error = port(h, &flags, 0);
    if (error)
        goto out;
    h->address = 0;
    h->ep0_size = 8;
    error = request(h, 0x80, 6, 0x100, 0, descriptor, 8, &count);
    if (error)
        goto out;
    if (count != 8 || descriptor[0] != 18 || descriptor[1] != 1 ||
        (h->speed == NOS_USB_LOW    ? descriptor[7] != 8
         : h->speed == NOS_USB_HIGH ? descriptor[7] != 64
                                    : descriptor[7] != 8 && descriptor[7] != 16 &&
                                          descriptor[7] != 32 && descriptor[7] != 64)) {
        error = -NOS_DRV_EIO;
        goto out;
    }
    h->ep0_size = descriptor[7];
    error = request(h, 0, 5, 1, 0, NULL, 0, &count);
    if (error)
        goto out;
    h->address = 1;
    h->ops->delay_us(h->controller.context, 2000);
    error = request(h, 0x80, 6, 0x100, 0, descriptor, 18, &count);
    if (error)
        goto out;
    if (count != 18 || descriptor[0] != 18 || descriptor[1] != 1 || descriptor[7] != h->ep0_size ||
        !descriptor[17]) {
        error = -NOS_DRV_EIO;
        goto out;
    }
    if (descriptor[4] == 9) {
        error = -NOS_DRV_ENOTSUP;
        goto out;
    }
    h->vendor = le16(descriptor + 8);
    h->product = le16(descriptor + 10);
    error = request(h, 0x80, 6, 0x200, 0, config, 9, &count);
    if (error)
        goto out;
    if (count != 9 || config[0] != 9 || config[1] != 2 || le16(config + 2) < 9 ||
        le16(config + 2) > NOS_USB_CONFIG_MAX) {
        error = -NOS_DRV_EINVAL;
        goto out;
    }
    error = request(h, 0x80, 6, 0x200, 0, h->configuration, le16(config + 2), &count);
    if (error)
        goto out;
    if (count != le16(config + 2)) {
        error = -NOS_DRV_EIO;
        goto out;
    }
    error = nos_usb_host_parse_configuration(h, h->configuration, count);
    if (error)
        goto out;
    error = request(h, 0, 9, h->configuration_value, 0, NULL, 0, &count);
    if (error)
        goto out;
    h->configured = 1;
    *handle = h->generation;
out:
    if (error)
        invalidate(h);
    nos_controller_end(&h->controller);
    return error;
}
static int handle_valid(struct nos_usb_host *h, uint32_t handle)
{
    return h->configured && handle && handle == h->generation ? 0 : -NOS_DRV_ENODEV;
}
int nos_usb_host_control(struct nos_usb_host *h, uint32_t handle, const uint8_t setup[8],
                         void *data, size_t length, size_t *actual, uint32_t timeout)
{
    int error;
    if (!setup || !actual || (length && !data) || length > 65535 || le16(setup + 6) != length ||
        !timeout || timeout > INT32_MAX)
        return -NOS_DRV_EINVAL;
    *actual = 0;
    /* Address, configuration, alternate setting and endpoint toggles are owned
     * by the core. Class/vendor requests remain available to class drivers. */
    if (!(setup[0] & 0x60) && (setup[1] == 5 || setup[1] == 9 || setup[1] == 11 ||
                               ((setup[0] & 0x1f) == 2 && (setup[1] == 1 || setup[1] == 3))))
        return -NOS_DRV_EINVAL;
    if ((error = begin(h)))
        return error;
    error = handle_valid(h, handle);
    if (!error)
        error = control(h, setup, data, length, actual, timeout);
    nos_controller_end(&h->controller);
    return error;
}
static struct nos_usb_endpoint *endpoint(struct nos_usb_host *h, uint8_t address)
{
    unsigned i;
    for (i = 0; i < h->endpoint_count; i++)
        if (h->endpoints[i].address == address)
            return &h->endpoints[i];
    return NULL;
}
int nos_usb_host_transfer(struct nos_usb_host *h, uint32_t handle, uint8_t address, void *data,
                          size_t length, size_t *actual, uint32_t timeout)
{
    struct nos_usb_endpoint *ep;
    uint32_t start;
    size_t count, done = 0;
    int error;
    if (!actual || (length && !data) || !timeout || timeout > INT32_MAX)
        return -NOS_DRV_EINVAL;
    *actual = 0;
    if ((error = begin(h)))
        return error;
    error = handle_valid(h, handle);
    if (error)
        goto out;
    ep = endpoint(h, address);
    if (!ep || length > h->controller.max_transfer) {
        error = -NOS_DRV_EINVAL;
        goto out;
    }
    if (ep->halted) {
        error = -NOS_USB_EPIPE;
        goto out;
    }
    start = h->ops->time_us(h->controller.context);
    if (ep->type == NOS_USB_INTERRUPT && ep->polled && (int32_t)(start - ep->next_poll_us) < 0) {
        error = -NOS_DRV_EAGAIN;
        goto out;
    }
    if (ep->type == NOS_USB_INTERRUPT && length > ep->max_packet) {
        error = -NOS_DRV_EINVAL;
        goto out;
    }
    do {
        struct nos_usb_packet p = {NULL,    0,        ep->max_packet, h->address,
                                   address, h->speed, ep->type,       ep->toggle};
        size_t part = length - done;
        if (part > ep->max_packet)
            part = ep->max_packet;
        p.data = length ? (uint8_t *)data + done : NULL;
        p.length = part;
        error = packet(h, &p, &count, start, timeout, ep->type != NOS_USB_INTERRUPT);
        if (ep->type == NOS_USB_INTERRUPT) {
            uint32_t interval =
                h->speed == NOS_USB_HIGH ? 125u << (ep->interval - 1) : ep->interval * 1000u;
            ep->next_poll_us = h->ops->time_us(h->controller.context) + interval;
            ep->polled = 1;
        }
        if (error) {
            if (error == -NOS_USB_EPIPE)
                ep->halted = 1;
            break;
        }
        ep->toggle ^= 1;
        done += count;
        if (count < part || ep->type == NOS_USB_INTERRUPT)
            break;
    } while (done < length);
    *actual = done;
out:
    nos_controller_end(&h->controller);
    return error;
}
int nos_usb_host_clear_halt(struct nos_usb_host *h, uint32_t handle, uint8_t address)
{
    struct nos_usb_endpoint *ep;
    size_t count;
    int error = begin(h);
    if (error)
        return error;
    error = handle_valid(h, handle);
    ep = endpoint(h, address);
    if (!error && !ep)
        error = -NOS_DRV_EINVAL;
    if (!error)
        error = request(h, 2, 1, 0, address, NULL, 0, &count);
    if (!error) {
        ep->halted = 0;
        ep->toggle = 0;
        ep->polled = 0;
    }
    nos_controller_end(&h->controller);
    return error;
}
