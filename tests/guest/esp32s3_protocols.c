/* Copyright (C) 2026 NOS contributors. Same protocol code on host and S3. */
#include <assert.h>
#include "esp_idf_test.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "nos/driver/ble.h"
#include "nos/driver/usb_class.h"
#include "nos/driver/usb_otg.h"

static void gatt_check(void)
{
    struct nos_ble_gatt *gatt = malloc(sizeof(*gatt));
    assert(gatt);
    const struct nos_ble_uuid services[] = {{2, {0x12, 0x18}}};
    uint8_t initial[] = {1, 2, 3};
    struct nos_ble_attribute attributes[] = {
        {.uuid = {2, {0x4d, 0x2a}},
         .id = 10,
         .flags = NOS_BLE_READ | NOS_BLE_WRITE | NOS_BLE_NOTIFY | NOS_BLE_INDICATE,
         .max_length = 64,
         .initial_value = initial,
         .initial_length = 3},
        {.uuid = {2, {8, 0x29}},
         .id = 11,
         .parent = 10,
         .flags = NOS_BLE_READ,
         .max_length = 2,
         .initial_value = initial,
         .initial_length = 2}};
    const struct nos_ble_profile profile = {
        "NOS GATT regression", services, attributes, 1, 2, NULL};
    assert(nos_ble_gatt_init(gatt, &profile) == 0);
    uint8_t value[64];
    size_t actual;
    assert(!nos_ble_gatt_get(gatt, 10, value, sizeof(value), &actual, 1) && actual == 3);
    assert(!memcmp(value, initial, 3));
    assert(nos_ble_gatt_set(gatt, 11, initial, 1, 1, 1) == -NOS_DRV_EACCES);
    assert(!nos_ble_gatt_peer(gatt, 7, 1));
    assert(nos_ble_gatt_can_notify(gatt, 7, 10, 3, 0) == -NOS_DRV_EACCES);
    assert(!nos_ble_gatt_subscribe(gatt, 7, 10, 1, 1));
    assert(!nos_ble_gatt_can_notify(gatt, 7, 10, 20, 0));
    assert(nos_ble_gatt_can_notify(gatt, 7, 10, 21, 0) == -NOS_DRV_EINVAL);
    assert(!nos_ble_gatt_mtu(gatt, 7, 80));
    assert(!nos_ble_gatt_can_notify(gatt, 7, 10, 64, 1));
    assert(nos_ble_gatt_can_notify(gatt, 7, 10, 65, 1) == -NOS_DRV_EINVAL);
    struct nos_ble_event event;
    while (!nos_ble_gatt_event(gatt, &event)) {
    }
    uint8_t update[] = {9, 8, 7, 6};
    assert(!nos_ble_gatt_set(gatt, 10, update, sizeof(update), 7, 1));
    assert(!nos_ble_gatt_event(gatt, &event) && event.type == NOS_BLE_WRITTEN &&
           event.attribute == 10 && event.connection == 7);
    assert(event.length == 4 && !memcmp(event.value, update, 4));
    for (unsigned i = 0; i < NOS_BLE_EVENTS; i++)
        assert(!nos_ble_gatt_set(gatt, 10, initial, 3, 7, 1));
    assert(nos_ble_gatt_set(gatt, 10, update, 4, 7, 1) == -NOS_DRV_ENOSPC);
    assert(!nos_ble_gatt_get(gatt, 10, value, sizeof(value), &actual, 0) && actual == 3 &&
           value[0] == 1);
    assert(gatt->dropped == 1);
    assert(!nos_ble_gatt_peer(gatt, 7, 0));
    assert(nos_ble_gatt_can_notify(gatt, 7, 10, 3, 0) == -NOS_DRV_ENODEV);
    assert(!nos_ble_gatt_peer(gatt, 7, 1));
    assert(nos_ble_gatt_can_notify(gatt, 7, 10, 3, 0) == -NOS_DRV_EACCES);
    uint8_t advertising[31];
    size_t length;
    assert(!nos_ble_advertising_data(&profile, advertising, &length));
    assert(length <= 31 && advertising[0] == 2 && advertising[1] == 1 && advertising[2] == 6);
    assert(advertising[4] == 3); /* Complete single-service UUID list. */
    const struct nos_ble_uuid more_services[] = {{2, {0x12, 0x18}}, {2, {0x0f, 0x18}}};
    struct nos_ble_profile advertised = profile;
    advertised.services = more_services;
    advertised.service_count = 2;
    assert(!nos_ble_advertising_data(&advertised, advertising, &length));
    assert(advertising[4] == 2); /* Multiple services: first UUID is incomplete. */
    attributes[1].uuid.value[0] = 2;
    assert(nos_ble_gatt_init(gatt, &profile) == -NOS_DRV_EINVAL); /* No duplicate CCCD. */
    attributes[1].uuid.value[0] = 8;
    attributes[1].parent = 99;
    assert(nos_ble_gatt_init(gatt, &profile) == -NOS_DRV_EINVAL);
    attributes[1].parent = 10;
    struct nos_ble_security invalid_security = {.io_capability = 3, .mitm = 1};
    struct nos_ble_profile invalid_profile = profile;
    invalid_profile.security = &invalid_security;
    assert(nos_ble_gatt_init(gatt, &invalid_profile) == -NOS_DRV_EINVAL);
    free(gatt);
    puts("PASS " NOS_IDF_TEST_NAME "_ble_gatt_state_descriptors_subscriptions_mtu_queue_atomicity");
}
static uint32_t live = 1;
static int control_error, transfer_error, malformed, controls;
static int describe(void *p, uint32_t handle, struct nos_usb_class_info *info)
{
    (void)p;
    if (handle != live)
        return -NOS_DRV_ENODEV;
    *info = (struct nos_usb_class_info){
        .max_transfer = 4096,
        .interface_count = 1,
        .endpoint_count = malformed ? 17 : 1,
        .interfaces = {{0, 3, 1, 1}},
        .endpoints = {
            {.address = 0x81, .type = NOS_USB_INTERRUPT, .max_packet = 8, .interval = 10}}};
    return 0;
}
static int control(void *p, uint32_t handle, const uint8_t setup[8], void *data, size_t n,
                   size_t *a, uint32_t t)
{
    (void)p;
    (void)t;
    (void)data;
    if (handle != live)
        return -NOS_DRV_ENODEV;
    controls++;
    *a = n;
    assert(setup[1] == 11 || setup[1] == 10 || setup[1] == 9);
    return control_error;
}
static int transfer(void *p, uint32_t handle, uint8_t ep, void *data, size_t n, size_t *a,
                    uint32_t t)
{
    (void)p;
    (void)t;
    if (handle != live)
        return -NOS_DRV_ENODEV;
    assert(ep == 0x81 && n == 8);
    const uint8_t report[8] = {0, 0, 4};
    memcpy(data, report, 8);
    *a = 8;
    return transfer_error;
}
static int clear(void *p, uint32_t h, uint8_t e)
{
    (void)p;
    (void)h;
    (void)e;
    return 0;
}
static void class_check(void)
{
    static const struct nos_usb_transport_ops ops = {describe, control, transfer, clear};
    const struct nos_usb_transport io = {&ops, NULL};
    struct nos_usb_hid hid;
    assert(!nos_usb_hid_open_transport(&hid, &io, live, 0) && controls == 2);
    assert(!nos_usb_hid_set_leds(&hid, 2));
    uint8_t report[8];
    size_t actual;
    assert(!nos_usb_hid_read(&hid, report, 8, &actual) && actual == 8 && report[2] == 4);
    live++;
    assert(nos_usb_hid_read(&hid, report, 8, &actual) == -NOS_DRV_ENODEV && !actual);
    control_error = 7;
    assert(nos_usb_hid_open_transport(&hid, &io, live, 0) == -NOS_DRV_EIO);
    control_error = 0;
    malformed = 1;
    assert(nos_usb_hid_open_transport(&hid, &io, live, 0) == -NOS_DRV_EIO);
    malformed = 0;
    assert(!nos_usb_hid_open_transport(&hid, &io, live, 0));
    transfer_error = 7;
    assert(nos_usb_hid_read(&hid, report, 8, &actual) == -NOS_DRV_EIO);
    transfer_error = 0;
    puts("PASS " NOS_IDF_TEST_NAME "_usb_class_transport_hid_stale_and_backend_errors");
}
static unsigned role, powered, starts, stops;
static int role_start(void *p, enum nos_usb_role r)
{
    (void)p;
    assert(!role);
    role = r;
    starts++;
    return 0;
}
static int role_stop(void *p, enum nos_usb_role r)
{
    (void)p;
    assert(role == (unsigned)r);
    role = 0;
    stops++;
    return 0;
}
static int vbus(void *p, int on)
{
    (void)p;
    powered = on;
    return 0;
}
static void otg_check(void)
{
    static const struct nos_usb_otg_ops ops = {role_stop, role_start, vbus};
    struct nos_usb_otg o = {.ops = &ops, .allowed = 3, .discharge_ms = 10};
    struct nos_usb_otg_inputs in = {.id_valid = 1, .id_grounded = 1, .source_allowed = 1};
    assert(!nos_usb_otg_request(&o, NOS_USB_ROLE_AUTO));
    assert(nos_usb_otg_update(&o, &in, UINT32_MAX - 5) == -NOS_DRV_EAGAIN && powered);
    in.vbus_present = 1;
    assert(!nos_usb_otg_update(&o, &in, 0) && role == NOS_USB_ROLE_HOST);
    in.id_grounded = 0;
    assert(nos_usb_otg_update(&o, &in, 1) == -NOS_DRV_EAGAIN && !powered && !role);
    assert(!nos_usb_otg_update(&o, &in, 12) && role == NOS_USB_ROLE_DEVICE);
    in.overcurrent = 1;
    assert(nos_usb_otg_update(&o, &in, 13) == -NOS_DRV_EIO && !role);
    assert(!nos_usb_otg_recover(&o, 14) && starts == 2 && stops == 2);
    puts("PASS " NOS_IDF_TEST_NAME "_usb_otg_auto_discharge_wrap_and_overcurrent");
}
void nos_esp32s3_protocol_regression(void)
{
    gatt_check();
    class_check();
    otg_check();
}
