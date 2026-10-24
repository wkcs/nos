/* Copyright (C) 2026 NOS contributors. NOS policy + IDF hardware backends. */
#include <stdlib.h>
#include <string.h>
#include "private.h"
#include "nos/driver/usb_service.h"
#include "driver/gpio.h"
#include "esp_timer.h"
struct otg_context {
    struct idf_device *device;
    struct nos_usb_otg policy;
    struct nos_usb_otg_inputs previous, stable;
    TaskHandle_t worker;
    uint32_t changed;
    uint8_t sampled, debounced;
};
static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}
static int stop_role(void *context, enum nos_usb_role role)
{
    struct otg_context *c = context;
    return (role == NOS_USB_ROLE_HOST ? &idf_usb_host_ops : &idf_usb_device_ops)->stop(c->device);
}
static int start_role(void *context, enum nos_usb_role role)
{
    struct otg_context *c = context;
    return (role == NOS_USB_ROLE_HOST ? &idf_usb_host_ops : &idf_usb_device_ops)->start(c->device);
}
static int power(void *context, int enabled)
{
    struct otg_context *c = context;
    const struct nos_idf_usb_otg_config *o = c->device->config.usb_otg;
    if (o->vbus_enable_pin < 0)
        return enabled ? -NOS_DRV_ENOTSUP : 0;
    return idf_error(gpio_set_level(o->vbus_enable_pin, !!enabled ^ o->enable_active_low));
}
static const struct nos_usb_otg_ops policy_ops = {stop_role, start_role, power};
static int input(int pin, int active_low)
{
    return pin >= 0 && (gpio_get_level(pin) ^ active_low);
}
static int update(struct otg_context *c)
{
    const struct nos_idf_usb_otg_config *o = c->device->config.usb_otg;
    struct nos_usb_otg_inputs in = {.id_valid = o->id_pin >= 0,
                                    .id_grounded = input(o->id_pin, o->id_active_low),
                                    .vbus_present = input(o->vbus_sense_pin, o->vbus_active_low),
                                    .source_allowed = o->source_allowed,
                                    .overcurrent =
                                        input(o->overcurrent_pin, o->overcurrent_active_low)};
    uint32_t now = now_ms();
    if (!c->sampled || memcmp(&in, &c->previous, sizeof(in))) {
        c->previous = in;
        c->changed = now;
        c->sampled = 1;
    }
    if ((uint32_t)(now - c->changed) >= (o->debounce_ms ? o->debounce_ms : 20)) {
        c->stable = in;
        c->debounced = 1;
    }
    /* Raw external VBUS must inhibit sourcing even before debounce. Likewise
     * loss of VBUS must quiesce an active role immediately. */
    if (!in.vbus_present)
        c->stable.vbus_present = 0;
    if (in.id_valid && !in.id_grounded) {
        c->stable.id_valid = 1;
        c->stable.id_grounded = 0;
    }
    c->stable.source_allowed =
        in.source_allowed && c->debounced && (!in.vbus_present || c->policy.sourcing);
    c->stable.overcurrent = in.overcurrent;
    return nos_usb_otg_update(&c->policy, &c->stable, now);
}
static void worker(void *context)
{
    struct otg_context *c = context;
    unsigned period = c->device->config.usb_otg->poll_ms;
    TickType_t delay = pdMS_TO_TICKS(period ? period : 10);
    if (!delay)
        delay = 1;
    for (;;) {
        uint32_t flags;
        (void)nos_service_call(&c->device->service, NOS_IO_POLL, &flags, sizeof(flags));
        vTaskDelay(delay);
    }
}
static int start(void *context)
{
    struct idf_device *d = context;
    const struct nos_idf_usb_otg_config *o = d->config.usb_otg;
    if (!o)
        return -NOS_DRV_EINVAL;
    struct otg_context *c = calloc(1, sizeof(*c));
    if (!c)
        return -NOS_DRV_ENOSPC;
    d->otg = c;
    c->device = d;
    c->policy = (struct nos_usb_otg){.ops = &policy_ops,
                                     .context = c,
                                     .allowed = o->allowed,
                                     .discharge_ms = o->discharge_ms,
                                     .power_timeout_ms = o->power_timeout_ms};
    if (o->vbus_enable_pin >= 0) {
        int rc = power(c, 0);
        if (rc)
            return rc;
        rc = idf_error(gpio_set_direction(o->vbus_enable_pin, GPIO_MODE_OUTPUT));
        if (rc)
            return rc;
    }
    int pins[] = {o->id_pin, o->vbus_sense_pin, o->overcurrent_pin};
    int active_low[] = {o->id_active_low, o->vbus_active_low, o->overcurrent_active_low};
    for (unsigned i = 0; i < 3; i++)
        if (pins[i] >= 0) {
            gpio_config_t config = {.pin_bit_mask = 1ULL << pins[i],
                                    .mode = GPIO_MODE_INPUT,
                                    .pull_up_en = active_low[i],
                                    .pull_down_en = !active_low[i]};
            int rc = idf_error(gpio_config(&config));
            if (rc)
                return rc;
        }
    if (xTaskCreate(worker, "nos-otg", 4096, c, 2, &c->worker) != pdPASS)
        return -NOS_DRV_ENOSPC;
    return 0;
}
static int stop(void *context)
{
    struct idf_device *d = context;
    struct otg_context *c = d->otg;
    if (!c)
        return 0;
    /* Domain mutex is held: worker cannot be inside a hardware request. */
    if (c->worker) {
        vTaskDelete(c->worker);
        c->worker = NULL;
    }
    int rc;
    if (c->policy.phase == NOS_USB_OTG_FAULT)
        rc = nos_usb_otg_recover(&c->policy, now_ms());
    else
        rc = nos_usb_otg_request(&c->policy, NOS_USB_ROLE_NONE);
    if (rc)
        return rc;
    rc = update(c);
    if (rc)
        return rc == -NOS_DRV_EAGAIN ? -NOS_DRV_EBUSY : rc;
    if (c->policy.active || c->policy.sourcing)
        return -NOS_DRV_EBUSY;
    free(c);
    d->otg = NULL;
    return 0;
}
static int call(void *context, uint32_t op, void *request, size_t size)
{
    struct idf_device *d = context;
    struct otg_context *c = d->otg;
    if (!c)
        return -NOS_DRV_ENODEV;
    if (op == NOS_IO_USB_ROLE && size == sizeof(enum nos_usb_role))
        return nos_usb_otg_request(&c->policy, *(enum nos_usb_role *)request);
    if (op == NOS_IO_USB_OTG_STATUS && size == sizeof(struct nos_usb_role_status)) {
        *(struct nos_usb_role_status *)request =
            (struct nos_usb_role_status){.inputs = c->stable,
                                         .generation = c->policy.generation,
                                         .error = c->policy.error,
                                         .requested = c->policy.requested,
                                         .active = c->policy.active,
                                         .phase = c->policy.phase,
                                         .sourcing = c->policy.sourcing};
        return 0;
    }
    if (op == NOS_IO_RECOVER && !size && c->policy.phase == NOS_USB_OTG_FAULT)
        return nos_usb_otg_recover(&c->policy, now_ms());
    if (op == NOS_IO_POLL && size == sizeof(uint32_t)) {
        int rc = update(c);
        *(uint32_t *)request = 0;
        if (rc && rc != -NOS_DRV_EAGAIN)
            return rc;
        if (c->policy.active == NOS_USB_ROLE_HOST && c->policy.phase == NOS_USB_OTG_ACTIVE)
            return idf_usb_host_ops.call(d, op, request, size);
        return rc;
    }
    if (c->policy.phase != NOS_USB_OTG_ACTIVE)
        return -NOS_DRV_EAGAIN;
    return (c->policy.active == NOS_USB_ROLE_HOST ? &idf_usb_host_ops : &idf_usb_device_ops)
        ->call(d, op, request, size);
}
const struct nos_service_ops idf_usb_otg_ops = {start, stop, call};
