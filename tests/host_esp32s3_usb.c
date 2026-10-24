/* Copyright (C) 2026 NOS contributors. Actual IDF bridge with async SDK model. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host_esp32s3_usb_sdk.h"
static usb_host_client_config_t client_config;
static usb_transfer_t *pending;
static int installed, client, opened, claimed, allocations, new_device, gone, root_off, canceling;
static int hang, fail_cancel, fail_root, stall, overflow, reject_submit;
static int64_t clock_us;
static int gpio_levels[49], worker_created, device_role;
static const uint8_t config[] = {9, 2, 25, 0, 1, 1, 0, 0x80, 50, 9, 4, 0, 0,
                                 1, 3, 1,  1, 0, 7, 5, 0x81, 3,  8, 0, 10};
static int device_token;
int idf_error(int e)
{
    return !e ? 0 : e == ESP_ERR_TIMEOUT ? -NOS_DRV_ETIMEDOUT : -NOS_DRV_EIO;
}
int64_t esp_timer_get_time(void)
{
    return clock_us;
}
void vTaskDelay(unsigned ticks)
{
    clock_us += ticks * 10000;
}
int xTaskCreate(void (*f)(void *), const char *name, unsigned stack, void *arg, unsigned priority,
                TaskHandle_t *out)
{
    assert(f && name && stack && arg && priority && !worker_created);
    worker_created = 1;
    *out = &worker_created;
    return pdPASS;
}
void vTaskDelete(TaskHandle_t t)
{
    assert(t == &worker_created && worker_created);
    worker_created = 0;
}
int gpio_set_level(int pin, int level)
{
    assert(pin >= 0 && pin < 49);
    gpio_levels[pin] = level;
    return 0;
}
int gpio_set_direction(int pin, int mode)
{
    assert(pin >= 0 && pin < 49 && mode == GPIO_MODE_OUTPUT);
    return 0;
}
int gpio_get_level(int pin)
{
    assert(pin >= 0 && pin < 49);
    return gpio_levels[pin];
}
int gpio_config(const gpio_config_t *c)
{
    assert(c->mode == GPIO_MODE_INPUT);
    return 0;
}
static int device_start(void *p)
{
    struct idf_device *d = p;
    assert(!installed && !device_role);
    d->installed = device_role = 1;
    return 0;
}
static int device_stop(void *p)
{
    struct idf_device *d = p;
    d->installed = device_role = 0;
    return 0;
}
static int device_call(void *p, uint32_t op, void *r, size_t n)
{
    (void)p;
    (void)op;
    (void)r;
    (void)n;
    return -NOS_DRV_ENOTSUP;
}
const struct nos_service_ops idf_usb_device_ops = {device_start, device_stop, device_call};
int usb_host_install(const usb_host_config_t *c)
{
    assert(c && !installed);
    installed = 1;
    root_off = 0;
    return 0;
}
int usb_host_uninstall(void)
{
    assert(installed && !client && !opened && !pending);
    installed = 0;
    return 0;
}
int usb_host_client_register(const usb_host_client_config_t *c, void **out)
{
    assert(!client);
    client_config = *c;
    client = 1;
    *out = &client;
    new_device = 1;
    return 0;
}
int usb_host_client_deregister(void *p)
{
    assert(p == &client && client && !opened && !pending);
    client = 0;
    return 0;
}
int usb_host_lib_handle_events(unsigned t, uint32_t *flags)
{
    (void)t;
    *flags = 0;
    return 0;
}
int usb_host_client_handle_events(void *p, unsigned t)
{
    (void)t;
    assert(p == &client && client);
    if (new_device) {
        usb_host_client_event_msg_t e = {.event = USB_HOST_CLIENT_EVENT_NEW_DEV, .new_dev = {1}};
        new_device = 0;
        client_config.async.client_event_callback(&e, client_config.async.callback_arg);
    }
    if (gone) {
        usb_host_client_event_msg_t e = {.event = USB_HOST_CLIENT_EVENT_DEV_GONE,
                                         .dev_gone = {&device_token}};
        gone = 0;
        client_config.async.client_event_callback(&e, client_config.async.callback_arg);
    }
    if (pending && (!hang || canceling || root_off)) {
        usb_transfer_t *x = pending;
        pending = NULL;
        x->status = root_off    ? USB_TRANSFER_STATUS_NO_DEVICE
                    : canceling ? USB_TRANSFER_STATUS_CANCELED
                    : stall     ? USB_TRANSFER_STATUS_STALL
                                : USB_TRANSFER_STATUS_COMPLETED;
        canceling = 0;
        if (!x->bEndpointAddress) {
            size_t n = x->data_buffer[6] | ((unsigned)x->data_buffer[7] << 8);
            x->actual_num_bytes = n + 8;
        } else {
            const uint8_t report[8] = {0, 0, 4};
            memcpy(x->data_buffer, report, 8);
            x->actual_num_bytes = overflow ? 9 : 8;
        }
        x->callback(x);
    }
    return 0;
}
int usb_host_lib_set_root_port_power(bool enabled)
{
    assert(!enabled);
    if (fail_root)
        return ESP_FAIL;
    root_off = 1;
    gone = 1;
    return 0;
}
int usb_host_device_open(void *p, uint8_t address, void **out)
{
    assert(p == &client && client && !opened && address == 1);
    opened = 1;
    *out = &device_token;
    return 0;
}
int usb_host_device_close(void *p, void *d)
{
    assert(p == &client && d == &device_token && opened && !claimed && !pending);
    opened = 0;
    return 0;
}
int usb_host_device_free_all(void)
{
    assert(!opened);
    return 0;
}
int usb_host_device_info(void *d, usb_device_info_t *out)
{
    assert(d == &device_token);
    out->speed = USB_SPEED_FULL;
    return 0;
}
int usb_host_get_device_descriptor(void *d, const usb_device_desc_t **out)
{
    static const usb_device_desc_t desc = {8, 0x1234, 0x5678};
    assert(d == &device_token);
    *out = &desc;
    return 0;
}
int usb_host_get_active_config_descriptor(void *d, const usb_config_desc_t **out)
{
    assert(d == &device_token);
    *out = (const usb_config_desc_t *)config;
    return 0;
}
int usb_host_interface_claim(void *p, void *d, uint8_t n, uint8_t alt)
{
    assert(p == &client && d == &device_token && !claimed && !n && !alt);
    claimed = 1;
    return 0;
}
int usb_host_interface_release(void *p, void *d, uint8_t n)
{
    assert(p == &client && d == &device_token && claimed && !n && !pending);
    claimed = 0;
    return 0;
}
int usb_host_endpoint_halt(void *d, uint8_t ep)
{
    assert(d == &device_token && ep);
    return fail_cancel ? ESP_FAIL : 0;
}
int usb_host_endpoint_flush(void *d, uint8_t ep)
{
    assert(d == &device_token && ep);
    if (fail_cancel)
        return ESP_FAIL;
    canceling = pending != NULL;
    return 0;
}
int usb_host_endpoint_clear(void *d, uint8_t ep)
{
    assert(d == &device_token && ep);
    stall = 0;
    return 0;
}
int usb_host_transfer_alloc(size_t size, int iso, usb_transfer_t **out)
{
    assert(!iso);
    usb_transfer_t *x = calloc(1, sizeof(*x));
    assert(x);
    x->data_buffer = malloc(size);
    assert(x->data_buffer);
    x->data_buffer_size = size;
    *out = x;
    allocations++;
    return 0;
}
int usb_host_transfer_free(usb_transfer_t *x)
{
    assert(!pending);
    free(x->data_buffer);
    free(x);
    allocations--;
    return 0;
}
int usb_host_transfer_submit(usb_transfer_t *x)
{
    assert(!pending && x && x->device_handle == &device_token);
    if (reject_submit)
        return ESP_FAIL;
    pending = x;
    return 0;
}
int usb_host_transfer_submit_control(void *p, usb_transfer_t *x)
{
    assert(p == &client && !x->bEndpointAddress);
    return usb_host_transfer_submit(x);
}
static int lock(void *p)
{
    (void)p;
    return 0;
}
static void unlock(void *p)
{
    (void)p;
}
static void check(unsigned scenario)
{
    assert(!allocations && !installed && !client && !opened && !claimed && !pending);
    hang = fail_cancel = fail_root = stall = overflow = reject_submit = 0;
    struct nos_service_domain domain;
    struct nos_device *entries[2];
    struct nos_service_lock sync = {NULL, lock, unlock};
    assert(!nos_service_domain_init(&domain, entries, 2, &sync));
    struct idf_device d = {0}, other = {0};
    const struct nos_resource resource = {.id = 8, .type = NOS_RESOURCE_CONTROLLER};
    assert(!nos_service_register(&domain, &d.service, "usb", NOS_SERVICE_USB_HOST, &resource, 1,
                                 &idf_usb_host_ops, &d));
    assert(!nos_service_register(&domain, &other.service, "other", NOS_SERVICE_USB_HOST, &resource,
                                 1, &idf_usb_host_ops, &other));
    assert(!nos_service_start(&d.service));
    struct nos_usb_attach a = {0};
    assert(!nos_service_call(&d.service, NOS_IO_USB_ATTACH, &a, sizeof(a)) && a.generation);
    struct nos_usb_class_open open = {a.generation, 0};
    if (scenario == 5) {
        hang = 1;
    } /* EP0 timeout uses root-port shutdown. */
    int rc = nos_service_call(&d.service, NOS_IO_USB_HID_OPEN, &open, sizeof(open));
    if (scenario == 5) {
        assert(rc == -NOS_DRV_ETIMEDOUT && root_off && !pending);
    } else {
        assert(!rc);
        uint8_t report[8];
        memset(report, 0xa5, sizeof(report));
        struct nos_usb_report r = {report, sizeof(report), 99, a.generation};
        if (scenario == 1 || scenario == 2)
            hang = 1;
        if (scenario == 2)
            fail_cancel = fail_root = 1;
        if (scenario == 3)
            overflow = 1;
        if (scenario == 4)
            reject_submit = 1;
        rc = nos_service_call(&d.service, NOS_IO_USB_HID_READ, &r, sizeof(r));
        if (scenario == 0)
            assert(!rc && r.actual == 8 && report[2] == 4);
        else {
            assert(rc == (scenario == 1   ? -NOS_DRV_EAGAIN
                          : scenario == 2 ? -NOS_DRV_ETIMEDOUT
                                          : -NOS_DRV_EIO) &&
                   !r.actual);
            for (unsigned i = 0; i < sizeof(report); i++)
                assert(report[i] == 0xa5);
            if (scenario == 1) {
                hang = 0;
                assert(!nos_service_call(&d.service, NOS_IO_USB_HID_READ, &r, sizeof(r)) &&
                       r.actual == 8 && report[2] == 4);
            }
        }
        if (scenario == 2) {
            assert(nos_service_stop(&d.service) != 0 && d.service.state == NOS_SERVICE_FAULTED &&
                   allocations == 1 && pending);
            assert(nos_service_start(&other.service) == -NOS_DRV_EBUSY);
            fail_cancel = fail_root = 0;
        }
        if (scenario == 0) {
            gone = 1;
            uint32_t flags;
            assert(!nos_service_call(&d.service, NOS_IO_POLL, &flags, sizeof(flags)));
            assert(nos_service_call(&d.service, NOS_IO_USB_HID_READ, &r, sizeof(r)) ==
                   -NOS_DRV_ENODEV);
        }
    }
    assert(!nos_service_stop(&d.service));
    assert(!nos_service_unregister(&d.service) && !nos_service_unregister(&other.service));
    assert(!allocations && !pending && !installed && !claimed && !opened && !client);
}
static void otg_check(void)
{
    hang = fail_cancel = fail_root = stall = overflow = reject_submit = 0;
    struct nos_service_domain domain;
    struct nos_device *entries[1];
    struct nos_service_lock sync = {NULL, lock, unlock};
    assert(!nos_service_domain_init(&domain, entries, 1, &sync));
    struct nos_idf_usb_otg_config board = {.id_pin = 4,
                                           .vbus_sense_pin = 5,
                                           .vbus_enable_pin = 6,
                                           .overcurrent_pin = 7,
                                           .allowed = 3,
                                           .source_allowed = 1,
                                           .id_active_low = 1,
                                           .discharge_ms = 10};
    struct idf_device d = {.config.usb_otg = &board};
    const struct nos_resource resource = {.id = 8, .type = NOS_RESOURCE_CONTROLLER};
    assert(!nos_service_register(&domain, &d.service, "otg", NOS_SERVICE_USB_OTG, &resource, 1,
                                 &idf_usb_otg_ops, &d));
    gpio_levels[4] = gpio_levels[5] = 1;
    gpio_levels[7] = 0;
    assert(!nos_service_start(&d.service) && worker_created && !gpio_levels[6]);
    enum nos_usb_role role = NOS_USB_ROLE_HOST;
    assert(!nos_service_call(&d.service, NOS_IO_USB_ROLE, &role, sizeof(role)));
    uint32_t flags;
    assert(nos_service_call(&d.service, NOS_IO_POLL, &flags, sizeof(flags)) == -NOS_DRV_EAGAIN);
    assert(!gpio_levels[6]); /* External VBUS blocks even during first debounce. */
    clock_us += 30000;
    assert(nos_service_call(&d.service, NOS_IO_POLL, &flags, sizeof(flags)) == -NOS_DRV_EAGAIN &&
           !gpio_levels[6]);
    role = NOS_USB_ROLE_AUTO;
    assert(!nos_service_call(&d.service, NOS_IO_USB_ROLE, &role, sizeof(role)));
    assert(!nos_service_call(&d.service, NOS_IO_POLL, &flags, sizeof(flags)) && device_role);
    gpio_levels[4] = 0;
    assert(!nos_service_call(&d.service, NOS_IO_POLL, &flags, sizeof(flags)));
    clock_us += 30000;
    assert(nos_service_call(&d.service, NOS_IO_POLL, &flags, sizeof(flags)) == -NOS_DRV_EAGAIN &&
           !device_role && !gpio_levels[6]);
    clock_us += 20000;
    gpio_levels[5] = 0;
    assert(nos_service_call(&d.service, NOS_IO_POLL, &flags, sizeof(flags)) == -NOS_DRV_EAGAIN &&
           gpio_levels[6] && !installed);
    gpio_levels[5] = 1;
    assert(nos_service_call(&d.service, NOS_IO_POLL, &flags, sizeof(flags)) == -NOS_DRV_EAGAIN);
    clock_us += 30000;
    assert(!nos_service_call(&d.service, NOS_IO_POLL, &flags, sizeof(flags)) && installed &&
           !device_role);
    gpio_levels[4] = 1;
    assert(nos_service_call(&d.service, NOS_IO_POLL, &flags, sizeof(flags)) == -NOS_DRV_EAGAIN);
    assert(!gpio_levels[6] && !installed);
    clock_us += 15000;
    assert(!nos_service_call(&d.service, NOS_IO_POLL, &flags, sizeof(flags)) && device_role);
    gpio_levels[7] = 1;
    assert(nos_service_call(&d.service, NOS_IO_POLL, &flags, sizeof(flags)) == -NOS_DRV_EIO);
    assert(!gpio_levels[6] && !installed && !allocations && !client);
    gpio_levels[7] = 0;
    assert(!nos_service_call(&d.service, NOS_IO_RECOVER, NULL, 0));
    clock_us += 30000;
    assert(!nos_service_stop(&d.service) && !worker_created);
    assert(!nos_service_unregister(&d.service));
    puts("PASS idf_otg_actual_adapter_debounce_no_backfeed_roles_overcurrent_worker_lifecycle");
}
int main(void)
{
    for (unsigned i = 0; i < 6; i++)
        check(i);
    otg_check();
    puts("PASS idf_usb_actual_bridge_async_hid_hotplug_short_overflow_timeout_quarantine_ep0");
}
