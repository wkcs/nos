/* Copyright (C) 2026 NOS contributors. */
#include "abi.h"
#include "driver/usb_host.h"
#include "driver/usb_otg.h"
struct guest_role {
    int active, powered, fail_stop;
};
static int role_start(void *context, enum nos_usb_role role)
{
    struct guest_role *r = context;
    if (r->active)
        return -NOS_DRV_EBUSY;
    r->active = role;
    return 0;
}
static int role_stop(void *context, enum nos_usb_role role)
{
    struct guest_role *r = context;
    if (r->active != (int)role || r->fail_stop)
        return -NOS_DRV_EIO;
    r->active = 0;
    return 0;
}
static int vbus(void *context, int on)
{
    ((struct guest_role *)context)->powered = on;
    return 0;
}
static const struct nos_usb_otg_ops ops = {role_stop, role_start, vbus};
void nos_usb_regression(void)
{
    struct guest_role role = {0};
    struct nos_usb_otg otg = {.ops = &ops, .context = &role, .allowed = 3};
    struct nos_usb_otg_inputs inputs = {1, 0, 1, 1, 0};
    struct nos_usb_host host = {.speed = NOS_USB_FULL, .power_budget_ma = 500};
    uint8_t config[] = {9, 2, 25, 0, 1, 1, 0, 0x80, 50, 9, 4, 0, 0,
                        1, 3, 1,  1, 0, 7, 5, 0x81, 3,  8, 0, 10};
    int ok = !nos_usb_host_parse_configuration(&host, config, sizeof(config));
    ok &=
        host.interface_count == 1 && host.endpoint_count == 1 && host.endpoints[0].max_packet == 8;
    config[24] = 0;
    ok &= nos_usb_host_parse_configuration(&host, config, sizeof(config)) == -NOS_DRV_EINVAL;
    ok &= host.endpoint_count == 1 && host.endpoints[0].interval == 10;
    ok &= !nos_usb_otg_request(&otg, NOS_USB_ROLE_AUTO);
    ok &= !nos_usb_otg_update(&otg, &inputs, 0) && role.active == NOS_USB_ROLE_DEVICE;
    inputs.id_grounded = 1;
    ok &= nos_usb_otg_update(&otg, &inputs, 1) == -NOS_DRV_EAGAIN && !role.active;
    ok &= nos_usb_otg_update(&otg, &inputs, 101) == -NOS_DRV_EAGAIN && !role.powered;
    inputs.vbus_present = 0;
    ok &= nos_usb_otg_update(&otg, &inputs, 102) == -NOS_DRV_EAGAIN && role.powered;
    inputs.vbus_present = 1;
    ok &= !nos_usb_otg_update(&otg, &inputs, 110) && role.active == NOS_USB_ROLE_HOST;
    inputs.overcurrent = 1;
    ok &= nos_usb_otg_update(&otg, &inputs, 120) == -NOS_DRV_EIO && !role.active && !role.powered;
    ok &= !nos_usb_otg_recover(&otg, 130) && otg.requested == NOS_USB_ROLE_NONE;
    nos_check(ok, "usb_host_descriptor_and_otg_userspace_roles");
}
