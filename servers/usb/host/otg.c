/* Copyright (C) 2026 NOS contributors. */
#include "driver/usb_otg.h"
static int valid(struct nos_usb_otg *o)
{
    return o && o->ops && o->ops->start && o->ops->stop && o->allowed && !(o->allowed & ~3u) &&
           (!(o->allowed & NOS_USB_ALLOW_HOST) || o->ops->set_vbus) &&
           o->phase <= NOS_USB_OTG_FAULT && o->active <= NOS_USB_ROLE_HOST &&
           o->requested <= NOS_USB_ROLE_AUTO;
}
static int allowed(struct nos_usb_otg *o, enum nos_usb_role role)
{
    return role == NOS_USB_ROLE_NONE || role == NOS_USB_ROLE_AUTO ||
           (o->allowed & (role == NOS_USB_ROLE_DEVICE ? NOS_USB_ALLOW_DEVICE : NOS_USB_ALLOW_HOST));
}
int nos_usb_otg_request(struct nos_usb_otg *o, enum nos_usb_role role)
{
    if (!valid(o) || (unsigned)role > NOS_USB_ROLE_AUTO)
        return -NOS_DRV_EINVAL;
    if (o->busy)
        return -NOS_DRV_EBUSY;
    if (!allowed(o, role))
        return -NOS_DRV_ENOTSUP;
    if (o->phase == NOS_USB_OTG_FAULT)
        return o->error ? o->error : -NOS_DRV_EIO;
    o->requested = (uint8_t)role;
    return 0;
}
static int quiesce(struct nos_usb_otg *o)
{
    int error = 0, off = 0;
    if (o->active) {
        error = nos_driver_status(o->ops->stop(o->context, (enum nos_usb_role)o->active));
        if (!error)
            o->active = NOS_USB_ROLE_NONE;
    }
    /* Remove source power even if the controller could not be stopped. */
    if (o->sourcing) {
        off = nos_driver_status(o->ops->set_vbus(o->context, 0));
        if (!off)
            o->sourcing = 0;
    }
    return error ? error : off;
}
static int fault(struct nos_usb_otg *o, int error)
{
    int stopped = quiesce(o);
    o->error = stopped ? stopped : error;
    o->phase = NOS_USB_OTG_FAULT;
    return o->error;
}
static int start_role(struct nos_usb_otg *o, enum nos_usb_role role)
{
    int error;
    /* Even a failed start may have partially enabled a role. */
    o->active = (uint8_t)role;
    error = nos_driver_status(o->ops->start(o->context, role));
    if (error)
        return fault(o, error);
    o->phase = NOS_USB_OTG_ACTIVE;
    return 0;
}
int nos_usb_otg_update(struct nos_usb_otg *o, const struct nos_usb_otg_inputs *in, uint32_t now)
{
    enum nos_usb_role target;
    uint32_t discharge, power_timeout;
    int error = 0;
    if (!valid(o) || !in || in->id_valid > 1 || in->id_grounded > 1 || in->vbus_present > 1 ||
        in->source_allowed > 1 || in->overcurrent > 1)
        return -NOS_DRV_EINVAL;
    if (o->busy)
        return -NOS_DRV_EBUSY;
    if (o->phase == NOS_USB_OTG_FAULT)
        return o->error ? o->error : -NOS_DRV_EIO;
    o->busy = 1;
    discharge = o->discharge_ms ? o->discharge_ms : 100;
    power_timeout = o->power_timeout_ms ? o->power_timeout_ms : 100;
    target = (enum nos_usb_role)o->requested;
    if (target == NOS_USB_ROLE_AUTO) {
        target = in->id_valid && in->id_grounded ? NOS_USB_ROLE_HOST
                 : in->vbus_present              ? NOS_USB_ROLE_DEVICE
                                                 : NOS_USB_ROLE_NONE;
        if (!allowed(o, target))
            target = NOS_USB_ROLE_NONE;
    }
    if (target == NOS_USB_ROLE_DEVICE && !in->vbus_present)
        target = NOS_USB_ROLE_NONE;
    if (in->overcurrent || (o->sourcing && !in->source_allowed)) {
        error = fault(o, -NOS_DRV_EIO);
        goto out;
    }
    if (o->phase == NOS_USB_OTG_ACTIVE && o->active == target) {
        if (target == NOS_USB_ROLE_HOST && !in->vbus_present)
            error = fault(o, -NOS_DRV_EIO);
        goto out;
    }
    if ((o->active && o->active != target) ||
        (o->phase == NOS_USB_OTG_POWER_WAIT && target != NOS_USB_ROLE_HOST)) {
        error = quiesce(o);
        if (error) {
            error = fault(o, error);
            goto out;
        }
        if (o->generation == UINT32_MAX) {
            error = fault(o, -NOS_DRV_ENOSPC);
            goto out;
        }
        o->generation++;
        o->changed_ms = now;
        o->phase = NOS_USB_OTG_DISCHARGE;
    }
    if (o->phase == NOS_USB_OTG_DISCHARGE) {
        if ((uint32_t)(now - o->changed_ms) < discharge) {
            error = -NOS_DRV_EAGAIN;
            goto out;
        }
        o->phase = NOS_USB_OTG_IDLE;
    }
    if (o->phase == NOS_USB_OTG_POWER_WAIT) {
        if (in->vbus_present)
            error = start_role(o, NOS_USB_ROLE_HOST);
        else if ((uint32_t)(now - o->changed_ms) >= power_timeout)
            error = fault(o, -NOS_DRV_ETIMEDOUT);
        else
            error = -NOS_DRV_EAGAIN;
        goto out;
    }
    if (!target)
        goto out;
    if (o->generation == UINT32_MAX) {
        error = fault(o, -NOS_DRV_ENOSPC);
        goto out;
    }
    if (target == NOS_USB_ROLE_DEVICE) {
        o->generation++;
        error = start_role(o, target);
    } else {
        /* Do not backfeed an already-powered cable, even on manual HOST request. */
        if (!in->source_allowed || in->vbus_present) {
            error = -NOS_DRV_EAGAIN;
            goto out;
        }
        error = nos_driver_status(o->ops->set_vbus(o->context, 1));
        if (error) {
            /* Treat failed enable as possibly energized; explicitly turn it off. */
            o->sourcing = 1;
            error = fault(o, error);
            goto out;
        }
        o->sourcing = 1;
        o->generation++;
        o->phase = NOS_USB_OTG_POWER_WAIT;
        o->changed_ms = now;
        error = -NOS_DRV_EAGAIN;
    }
out:
    o->busy = 0;
    return error;
}
int nos_usb_otg_recover(struct nos_usb_otg *o, uint32_t now)
{
    int error;
    if (!valid(o))
        return -NOS_DRV_EINVAL;
    if (o->busy)
        return -NOS_DRV_EBUSY;
    o->busy = 1;
    error = quiesce(o);
    if (error) {
        o->phase = NOS_USB_OTG_FAULT;
        o->error = error;
    } else if (o->generation == UINT32_MAX) {
        o->phase = NOS_USB_OTG_FAULT;
        o->error = error = -NOS_DRV_ENOSPC;
    } else {
        o->requested = NOS_USB_ROLE_NONE;
        o->generation++;
        o->changed_ms = now;
        o->phase = NOS_USB_OTG_DISCHARGE;
        o->error = 0;
    }
    o->busy = 0;
    return error;
}
