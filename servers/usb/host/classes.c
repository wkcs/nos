/* Copyright (C) 2026 NOS contributors. */
#include "driver/usb_class.h"
static int describe(const struct nos_usb_transport *io, uint32_t handle,
                    struct nos_usb_class_info *info)
{
    if (!io || !io->ops || !io->ops->describe || !io->ops->control || !io->ops->transfer ||
        !io->ops->clear_halt || !handle)
        return -NOS_DRV_EINVAL;
    int rc = nos_driver_status(io->ops->describe(io->context, handle, info));
    if (rc)
        return rc;
    if (info->interface_count > NOS_USB_INTERFACES || info->endpoint_count > NOS_USB_ENDPOINTS ||
        !info->max_transfer || info->max_transfer > NOS_DRIVER_MAX_TRANSFER)
        return -NOS_DRV_EIO;
    return 0;
}
static struct nos_usb_interface *interface(struct nos_usb_class_info *info, uint8_t number)
{
    for (unsigned i = 0; i < info->interface_count; i++)
        if (info->interfaces[i].number == number)
            return &info->interfaces[i];
    return NULL;
}
static int native_describe(void *context, uint32_t handle, struct nos_usb_class_info *info)
{
    struct nos_usb_host *h = context;
    if (!h || !h->configured || !handle || handle != h->generation)
        return -NOS_DRV_ENODEV;
    if (h->interface_count > NOS_USB_INTERFACES || h->endpoint_count > NOS_USB_ENDPOINTS)
        return -NOS_DRV_EIO;
    *info = (struct nos_usb_class_info){.max_transfer = h->controller.max_transfer,
                                        .interface_count = h->interface_count,
                                        .endpoint_count = h->endpoint_count};
    for (unsigned i = 0; i < h->interface_count; i++)
        info->interfaces[i] = h->interfaces[i];
    for (unsigned i = 0; i < h->endpoint_count; i++)
        info->endpoints[i] = h->endpoints[i];
    return 0;
}
static int native_control(void *c, uint32_t g, const uint8_t s[8], void *d, size_t n, size_t *a,
                          uint32_t t)
{
    return nos_usb_host_control(c, g, s, d, n, a, t);
}
static int native_transfer(void *c, uint32_t g, uint8_t e, void *d, size_t n, size_t *a, uint32_t t)
{
    return nos_usb_host_transfer(c, g, e, d, n, a, t);
}
static int native_clear(void *c, uint32_t g, uint8_t e)
{
    return nos_usb_host_clear_halt(c, g, e);
}
static const struct nos_usb_transport_ops native_ops = {native_describe, native_control,
                                                        native_transfer, native_clear};
static int class_transfer(const struct nos_usb_transport *io, uint32_t g, uint8_t ep, void *data,
                          size_t length, size_t *actual, uint32_t timeout)
{
    *actual = 0;
    int rc =
        nos_driver_status(io->ops->transfer(io->context, g, ep, data, length, actual, timeout));
    if (!rc && *actual > length)
        rc = -NOS_DRV_EIO;
    return rc;
}
static int class_request(const struct nos_usb_transport *io, uint32_t handle, uint8_t type,
                         uint8_t req, uint16_t value, uint16_t index, void *data, uint16_t length,
                         size_t *actual)
{
    uint8_t setup[8] = {type,
                        req,
                        (uint8_t)value,
                        (uint8_t)(value >> 8),
                        (uint8_t)index,
                        (uint8_t)(index >> 8),
                        (uint8_t)length,
                        (uint8_t)(length >> 8)};
    *actual = 0;
    int rc = nos_driver_status(
        io->ops->control(io->context, handle, setup, data, length, actual, 500000));
    return !rc && *actual > length ? -NOS_DRV_EIO : rc;
}
int nos_usb_hid_open_transport(struct nos_usb_hid *hid, const struct nos_usb_transport *io,
                               uint32_t handle, uint8_t number)
{
    struct nos_usb_class_info info;
    int checked = describe(io, handle, &info);
    if (checked)
        return checked;
    struct nos_usb_interface *iface = interface(&info, number);
    struct nos_usb_hid candidate = {.handle = handle, .interface_number = number, .io = *io};
    size_t actual;
    unsigned i;
    int error;
    if (!hid)
        return -NOS_DRV_EINVAL;
    if (!iface)
        return -NOS_DRV_ENODEV;
    if (iface->class_code != 3 || iface->subclass != 1 ||
        (iface->protocol != 1 && iface->protocol != 2))
        return -NOS_DRV_ENOTSUP;
    candidate.protocol = iface->protocol;
    for (i = 0; i < info.endpoint_count; i++) {
        struct nos_usb_endpoint *ep = &info.endpoints[i];
        if (ep->interface_number == number && ep->type == NOS_USB_INTERRUPT &&
            (ep->address & 0x80) && ep->max_packet >= (iface->protocol == 1 ? 8 : 3)) {
            candidate.endpoint = ep->address;
            break;
        }
    }
    if (!candidate.endpoint)
        return -NOS_DRV_ENOTSUP;
    error =
        class_request(io, handle, 0x21, 11, 0, number, NULL, 0, &actual); /* SET_PROTOCOL(boot) */
    if (error)
        return error;
    error = class_request(io, handle, 0x21, 10, 0, number, NULL, 0, &actual); /* SET_IDLE */
    if (error && error != -NOS_USB_EPIPE)
        return error;
    *hid = candidate;
    return 0;
}
int nos_usb_hid_read(struct nos_usb_hid *hid, void *data, size_t capacity, size_t *actual)
{
    uint8_t report[8];
    size_t length, received = 0, requested = 0;
    int error;
    if (!hid || !data || !actual)
        return -NOS_DRV_EINVAL;
    *actual = 0;
    length = hid->protocol == 1 ? 8 : hid->protocol == 2 ? 3 : 0;
    if (!length || capacity < length)
        return -NOS_DRV_EINVAL;
    struct nos_usb_class_info info;
    int checked = describe(&hid->io, hid->handle, &info);
    if (checked)
        return checked;
    if (!interface(&info, hid->interface_number))
        return -NOS_DRV_ENODEV;
    for (unsigned i = 0; i < info.endpoint_count; i++) {
        struct nos_usb_endpoint *ep = &info.endpoints[i];
        if (ep->address == hid->endpoint) {
            requested = ep->max_packet;
            break;
        }
    }
    if (requested > sizeof(report))
        requested = sizeof(report);
    if (requested < length)
        return -NOS_DRV_EINVAL;
    error =
        class_transfer(&hid->io, hid->handle, hid->endpoint, report, requested, &received, 100000);
    if (error)
        return error;
    /* Some wheel mice (including QEMU) retain a fourth byte in boot protocol.
     * Receive safely at endpoint size, normalize the three-byte boot report. */
    if (hid->protocol == 1 ? received != 8 : (received != 3 && received != 4))
        return -NOS_DRV_EIO;
    for (size_t i = 0; i < length; i++)
        ((uint8_t *)data)[i] = report[i];
    *actual = length;
    return 0;
}
int nos_usb_hid_set_leds(struct nos_usb_hid *hid, uint8_t leds)
{
    size_t actual;
    int error;
    if (!hid || hid->protocol != 1 || (leds & 0xe0))
        return -NOS_DRV_EINVAL;
    error = class_request(&hid->io, hid->handle, 0x21, 9, 0x200, hid->interface_number, &leds, 1,
                          &actual);
    return !error && actual != 1 ? -NOS_DRV_EIO : error;
}
static void put32(uint8_t *p, uint32_t n)
{
    p[0] = (uint8_t)n;
    p[1] = (uint8_t)(n >> 8);
    p[2] = (uint8_t)(n >> 16);
    p[3] = (uint8_t)(n >> 24);
}
static uint32_t get32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void big32(uint8_t *p, uint32_t n)
{
    p[0] = (uint8_t)(n >> 24);
    p[1] = (uint8_t)(n >> 16);
    p[2] = (uint8_t)(n >> 8);
    p[3] = (uint8_t)n;
}
static uint32_t from_big32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static int bot(struct nos_usb_msc *m, const uint8_t *command, uint8_t command_length, void *data,
               uint32_t length, int input)
{
    uint8_t cbw[31] = {0}, csw[13];
    size_t actual = 0, completed = 0;
    int error;
    if (!m || !m->io.ops || m->blocked)
        return -NOS_DRV_EIO;
    if (m->tag == UINT32_MAX)
        return -NOS_DRV_ENOSPC;
    put32(cbw, 0x43425355);
    put32(cbw + 4, ++m->tag);
    put32(cbw + 8, length);
    cbw[12] = input ? 0x80 : 0;
    cbw[14] = command_length;
    for (unsigned i = 0; i < command_length; i++)
        cbw[15 + i] = command[i];
    error = class_transfer(&m->io, m->handle, m->out_endpoint, cbw, sizeof(cbw), &actual, 1000000);
    if (error || actual != sizeof(cbw))
        goto blocked;
    if (length) {
        error = class_transfer(&m->io, m->handle, input ? m->in_endpoint : m->out_endpoint, data,
                               length, &completed, 1000000);
        /* A failed data phase requires explicit BOT reset, never command replay. */
        if (error)
            goto blocked;
    }
    error = class_transfer(&m->io, m->handle, m->in_endpoint, csw, sizeof(csw), &actual, 1000000);
    if (error || actual != sizeof(csw) || get32(csw) != 0x53425355 || get32(csw + 4) != m->tag ||
        csw[12] > 2 || get32(csw + 8) > length)
        goto blocked;
    if (csw[12] == 2 || completed + get32(csw + 8) != length)
        goto blocked;
    return csw[12] ? -NOS_DRV_EIO : 0;
blocked:
    m->blocked = 1;
    return error ? error : -NOS_DRV_EIO;
}
int nos_usb_msc_recover(struct nos_usb_msc *m)
{
    size_t actual;
    int error;
    if (!m || !m->io.ops)
        return -NOS_DRV_EINVAL;
    m->blocked = 1;
    error = class_request(&m->io, m->handle, 0x21, 0xff, 0, m->interface_number, NULL, 0, &actual);
    if (!error)
        error = nos_driver_status(m->io.ops->clear_halt(m->io.context, m->handle, m->in_endpoint));
    if (!error)
        error = nos_driver_status(m->io.ops->clear_halt(m->io.context, m->handle, m->out_endpoint));
    if (!error)
        m->blocked = 0;
    return error;
}
int nos_usb_msc_open_transport(struct nos_usb_msc *m, const struct nos_usb_transport *io,
                               uint32_t handle, uint8_t number)
{
    struct nos_usb_class_info info;
    int checked = describe(io, handle, &info);
    if (checked)
        return checked;
    struct nos_usb_interface *iface = interface(&info, number);
    uint8_t capacity[8], command[10] = {0x25}, lun = 0;
    size_t actual;
    unsigned i;
    int error;
    if (!m)
        return -NOS_DRV_EINVAL;
    if (!iface)
        return -NOS_DRV_ENODEV;
    if (iface->class_code != 8 || iface->subclass != 6 || iface->protocol != 0x50)
        return -NOS_DRV_ENOTSUP;
    *m = (struct nos_usb_msc){
        .handle = handle, .interface_number = number, .io = *io, .max_transfer = info.max_transfer};
    for (i = 0; i < info.endpoint_count; i++) {
        struct nos_usb_endpoint *ep = &info.endpoints[i];
        if (ep->interface_number != number || ep->type != NOS_USB_BULK)
            continue;
        if (ep->address & 0x80)
            m->in_endpoint = ep->address;
        else
            m->out_endpoint = ep->address;
    }
    if (!m->in_endpoint || !m->out_endpoint)
        return -NOS_DRV_ENOTSUP;
    error = class_request(io, handle, 0xa1, 0xfe, 0, number, &lun, 1, &actual);
    if (error != -NOS_USB_EPIPE && (error || actual != 1))
        return error ? error : -NOS_DRV_EIO;
    if (!error && lun > 15)
        return -NOS_DRV_EINVAL;
    {
        uint8_t ready[6] = {0};
        error = bot(m, ready, 6, NULL, 0, 0); /* TEST UNIT READY consumes power-on attention. */
        if (error && !m->blocked) {
            uint8_t sense[18], sense_command[6] = {3, 0, 0, 0, 18, 0};
            error = bot(m, sense_command, 6, sense, sizeof(sense), 1);
        }
        if (error)
            return error;
    }
    error = bot(m, command, 10, capacity, 8, 1);
    if (error && !m->blocked) {
        /* Clear one power-on unit attention with REQUEST SENSE. No data-write
         * command is retried, only the read-only capacity query. */
        uint8_t sense[18], sense_command[6] = {3, 0, 0, 0, 18, 0};
        error = bot(m, sense_command, 6, sense, sizeof(sense), 1);
        if (!error)
            error = bot(m, command, 10, capacity, 8, 1);
    }
    if (error)
        return error;
    if (from_big32(capacity) == UINT32_MAX)
        return -NOS_DRV_ENOTSUP;
    uint32_t block_size = from_big32(capacity + 4);
    if (block_size < 512 || block_size > 4096 || (block_size & (block_size - 1)))
        return -NOS_DRV_ENOTSUP;
    m->blocks = from_big32(capacity) + 1;
    m->block_size = block_size;
    return 0;
}
static int blocks(struct nos_usb_msc *m, uint32_t lba, uint16_t count, void *data, int input)
{
    uint8_t command[10] = {0};
    uint32_t length;
    if (!m || !m->io.ops || !m->block_size || !count || !data || lba >= m->blocks ||
        count > m->blocks - lba || count > m->max_transfer / m->block_size)
        return -NOS_DRV_EINVAL;
    length = (uint32_t)count * m->block_size;
    command[0] = input ? 0x28 : 0x2a;
    big32(command + 2, lba);
    command[7] = (uint8_t)(count >> 8);
    command[8] = (uint8_t)count;
    return bot(m, command, 10, data, length, input);
}
int nos_usb_msc_read(struct nos_usb_msc *m, uint32_t lba, uint16_t count, void *data)
{
    return blocks(m, lba, count, data, 1);
}
int nos_usb_msc_write(struct nos_usb_msc *m, uint32_t lba, uint16_t count, const void *data)
{
    return blocks(m, lba, count, (void *)data, 0);
}
int nos_usb_msc_sync(struct nos_usb_msc *m)
{
    const uint8_t command[10] = {0x35};
    return bot(m, command, 10, NULL, 0, 0);
}

int nos_usb_hid_open(struct nos_usb_hid *hid, struct nos_usb_host *h, uint32_t g, uint8_t n)
{
    struct nos_usb_transport io = {&native_ops, h};
    int rc = nos_usb_hid_open_transport(hid, &io, g, n);
    if (!rc)
        hid->host = h;
    return rc;
}
int nos_usb_msc_open(struct nos_usb_msc *msc, struct nos_usb_host *h, uint32_t g, uint8_t n)
{
    struct nos_usb_transport io = {&native_ops, h};
    int rc = nos_usb_msc_open_transport(msc, &io, g, n);
    if (!rc)
        msc->host = h;
    return rc;
}
