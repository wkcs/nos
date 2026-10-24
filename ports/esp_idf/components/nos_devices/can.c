/* Copyright (C) 2026 NOS contributors. Classic CAN via IDF TWAI, no CAN-FD. */
#include <string.h>
#include "private.h"
#include "driver/twai.h"
static int can_start(void *context)
{
    struct idf_device *d = context;
    twai_general_config_t config =
        TWAI_GENERAL_CONFIG_DEFAULT(d->config.pins[0], d->config.pins[1], TWAI_MODE_NORMAL);
    twai_timing_config_t timing;
    switch (d->config.hz) {
    case 125000:
        timing = (twai_timing_config_t)TWAI_TIMING_CONFIG_125KBITS();
        break;
    case 250000:
        timing = (twai_timing_config_t)TWAI_TIMING_CONFIG_250KBITS();
        break;
    case 500000:
        timing = (twai_timing_config_t)TWAI_TIMING_CONFIG_500KBITS();
        break;
    default:
        timing = (twai_timing_config_t)TWAI_TIMING_CONFIG_1MBITS();
        break;
    }
    twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    int rc = idf_error(twai_driver_install(&config, &timing, &filter));
    if (!rc) {
        d->installed = 1;
        rc = idf_error(twai_start());
    }
    return rc;
}
static int can_stop(void *context)
{
    struct idf_device *d = context;
    if (!d->installed)
        return 0;
    twai_status_info_t info;
    int rc = idf_error(twai_get_status_info(&info));
    if (rc)
        return rc;
    if (info.state == TWAI_STATE_RUNNING) {
        rc = idf_error(twai_stop());
        if (rc)
            return rc;
    }
    rc = idf_error(twai_driver_uninstall());
    if (!rc)
        d->installed = 0;
    return rc;
}
static int can_call(void *context, uint32_t op, void *request, size_t size)
{
    (void)context;
    if (op == NOS_IO_STATUS && size == sizeof(struct nos_io_can_status)) {
        twai_status_info_t info;
        int rc = idf_error(twai_get_status_info(&info));
        if (rc)
            return rc;
        *(struct nos_io_can_status *)request =
            (struct nos_io_can_status){info.bus_error_count, info.msgs_to_tx, info.msgs_to_rx,
                                       info.state == TWAI_STATE_BUS_OFF};
        return 0;
    }
    if ((op == NOS_IO_READ || op == NOS_IO_WRITE) && size == sizeof(struct nos_io_can_frame)) {
        struct nos_io_can_frame *f = request;
        twai_message_t message = {0};
        int result;
        if (op == NOS_IO_WRITE) {
            if (f->length > 8 || f->flags & ~(NOS_CAN_EXTENDED | NOS_CAN_REMOTE) ||
                f->identifier > ((f->flags & NOS_CAN_EXTENDED) ? 0x1fffffffu : 0x7ffu))
                return -NOS_DRV_EINVAL;
            message.identifier = f->identifier;
            message.data_length_code = f->length;
            message.extd = !!(f->flags & NOS_CAN_EXTENDED);
            message.rtr = !!(f->flags & NOS_CAN_REMOTE);
            memcpy(message.data, f->data, f->length);
            result = twai_transmit(&message, 0);
        } else {
            result = twai_receive(&message, 0);
            if (result == ESP_OK) {
                if (message.data_length_code > 8)
                    return -NOS_DRV_EIO;
                *f = (struct nos_io_can_frame){.identifier = message.identifier,
                                               .flags = (message.extd ? NOS_CAN_EXTENDED : 0) |
                                                        (message.rtr ? NOS_CAN_REMOTE : 0),
                                               .length = message.data_length_code};
                memcpy(f->data, message.data, f->length);
            }
        }
        return result == ESP_ERR_TIMEOUT ? -NOS_DRV_EAGAIN : idf_error(result);
    }
    return -NOS_DRV_ENOTSUP;
}
const struct nos_service_ops idf_can_ops = {can_start, can_stop, can_call};
