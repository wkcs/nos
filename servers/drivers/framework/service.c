/* Copyright (C) 2026 NOS contributors. */
#include "driver/service.h"

static int enter(struct nos_service *s)
{
    if (!s || !s->domain)
        return -NOS_DRV_EINVAL;
    return nos_driver_status(s->domain->lock.enter(s->domain->lock.context));
}
static void leave(struct nos_service *s)
{
    s->domain->lock.leave(s->domain->lock.context);
}
static int claim(void *context, const struct nos_resource *r, uint64_t *token)
{
    struct nos_service_domain *d = context;
    unsigned free_slot = NOS_SERVICE_CLAIMS;
    for (unsigned i = 0; i < NOS_SERVICE_CLAIMS; i++) {
        if (d->claims[i].owned && d->claims[i].type == r->type && d->claims[i].id == r->id)
            return -NOS_DRV_EBUSY;
        if (!d->claims[i].owned && free_slot == NOS_SERVICE_CLAIMS)
            free_slot = i;
    }
    if (free_slot == NOS_SERVICE_CLAIMS)
        return -NOS_DRV_ENOSPC;
    d->claims[free_slot].type = r->type;
    d->claims[free_slot].id = r->id;
    d->claims[free_slot].owned = 1;
    *token = free_slot + 1;
    return 0;
}
static void release(void *context, const struct nos_resource *r, uint64_t token)
{
    struct nos_service_domain *d = context;
    (void)r;
    if (token && token <= NOS_SERVICE_CLAIMS)
        d->claims[token - 1].owned = 0;
}
static int probe(struct nos_device *d)
{
    struct nos_service *s = d->context;
    for (unsigned i = 0; i < d->resource_count; i++) {
        int rc = nos_device_acquire_resource(d, d->resources[i].type, d->resources[i].id,
                                             &s->domain->provider, &s->leases[i]);
        if (rc)
            return rc;
    }
    return 0;
}
static void remove_device(struct nos_device *d)
{
    (void)d; /* SDK stop has succeeded before unbind; framework releases claims. */
}
static const struct nos_driver driver = {"nos,managed-service", probe, remove_device, NULL, NULL};
int nos_service_domain_init(struct nos_service_domain *d, struct nos_device **entries,
                            size_t capacity, const struct nos_service_lock *lock)
{
    if (!d || !entries || !capacity || !lock || !lock->enter || !lock->leave)
        return -NOS_DRV_EINVAL;
    *d = (struct nos_service_domain){.registry = {entries, capacity, 0}, .lock = *lock};
    d->provider = (struct nos_resource_provider){d, claim, release};
    return 0;
}
int nos_service_register(struct nos_service_domain *d, struct nos_service *s, const char *name,
                         enum nos_service_kind kind, const struct nos_resource *resources,
                         size_t count, const struct nos_service_ops *ops, void *context)
{
    int rc;
    if (!d || !d->lock.enter || !d->lock.leave || !s || s->domain ||
        (unsigned)kind >= NOS_SERVICE_KIND_COUNT || count > NOS_DRIVER_MAX_RESOURCES ||
        (count && !resources) || !ops || !ops->start || !ops->stop)
        return -NOS_DRV_EINVAL;
    /* These claims arbitrate logical ownership only. MMIO/IRQ/DMA permission
     * must use an authorized platform provider, never this bookkeeping pool. */
    for (size_t i = 0; i < count; i++)
        if (resources[i].type != NOS_RESOURCE_PINCTRL &&
            resources[i].type != NOS_RESOURCE_CONTROLLER)
            return -NOS_DRV_ENOTSUP;
    if (!name || !*name)
        return -NOS_DRV_EINVAL;
    size_t length = 0;
    while (name[length] && length <= NOS_SERVICE_NAME_MAX)
        length++;
    if (length > NOS_SERVICE_NAME_MAX)
        return -NOS_DRV_EINVAL;
    if ((rc = nos_driver_status(d->lock.enter(d->lock.context))))
        return rc;
    s->device = (struct nos_device){.name = name,
                                    .compatible = driver.compatible,
                                    .resources = resources,
                                    .resource_count = count,
                                    .context = s};
    rc = nos_device_register(&d->registry, &s->device);
    if (!rc) {
        s->domain = d;
        s->ops = ops;
        s->context = context;
        s->kind = kind;
        s->state = NOS_SERVICE_STOPPED;
    }
    d->lock.leave(d->lock.context);
    return rc;
}
int nos_service_unregister(struct nos_service *s)
{
    int rc = enter(s);
    if (rc)
        return rc;
    struct nos_service_domain *d = s->domain;
    rc = s->state != NOS_SERVICE_STOPPED || s->in_call
             ? -NOS_DRV_EBUSY
             : nos_device_unregister(&d->registry, &s->device);
    if (!rc)
        *s = (struct nos_service){0};
    d->lock.leave(d->lock.context);
    return rc;
}
int nos_service_start(struct nos_service *s)
{
    int rc = enter(s);
    if (rc)
        return rc;
    if (s->state != NOS_SERVICE_STOPPED || s->in_call) {
        rc = -NOS_DRV_EBUSY;
        goto out;
    }
    s->in_call = 1;
    rc = nos_device_bind(&s->device, &driver);
    if (!rc) {
        /* Retain a reference until SDK teardown succeeds. */
        rc = nos_device_get(&s->device);
        if (!rc) {
            rc = nos_driver_status(s->ops->start(s->context));
            if (!rc)
                s->state = NOS_SERVICE_RUNNING;
            else if (nos_driver_status(s->ops->stop(s->context)))
                s->state = NOS_SERVICE_FAULTED;
            else {
                nos_device_put(&s->device);
                nos_device_unbind(&s->device);
            }
        } else
            nos_device_unbind(&s->device);
    }
    s->in_call = 0;
out:
    leave(s);
    return rc;
}
int nos_service_stop(struct nos_service *s)
{
    int rc = enter(s);
    if (rc)
        return rc;
    if (s->state == NOS_SERVICE_STOPPED) {
        rc = -NOS_DRV_ENODEV;
        goto out;
    }
    if (s->in_call || s->device.users != 1) {
        rc = -NOS_DRV_EBUSY;
        goto out;
    }
    s->in_call = 1;
    rc = nos_driver_status(s->ops->stop(s->context));
    if (rc)
        s->state = NOS_SERVICE_FAULTED;
    else {
        nos_device_put(&s->device);
        rc = nos_device_unbind(&s->device);
        s->state = NOS_SERVICE_STOPPED;
    }
    s->in_call = 0;
out:
    leave(s);
    return rc;
}
int nos_service_call(struct nos_service *s, uint32_t command, void *request, size_t size)
{
    int rc = enter(s);
    if (rc)
        return rc;
    if (s->in_call)
        rc = -NOS_DRV_EBUSY;
    else if (s->state != NOS_SERVICE_RUNNING)
        rc = -NOS_DRV_ENODEV;
    else if (!s->ops->call)
        rc = -NOS_DRV_ENOTSUP;
    else if (size && !request)
        rc = -NOS_DRV_EINVAL;
    else {
        s->in_call = 1;
        rc = nos_device_get(&s->device);
        if (!rc) {
            rc = nos_driver_status(s->ops->call(s->context, command, request, size));
            nos_device_put(&s->device);
        }
        s->in_call = 0;
    }
    leave(s);
    return rc;
}
int nos_service_snapshot(struct nos_service_domain *d, struct nos_service_info *info,
                         size_t capacity, size_t *count)
{
    int rc;
    if (!d || !d->lock.enter || !d->lock.leave || !count || (capacity && !info))
        return -NOS_DRV_EINVAL;
    if ((rc = nos_driver_status(d->lock.enter(d->lock.context))))
        return rc;
    *count = d->registry.count;
    if (capacity < *count)
        rc = -NOS_DRV_ENOSPC;
    else {
        for (size_t i = 0; i < *count; i++) {
            struct nos_service *s = d->registry.entries[i]->context;
            info[i] = (struct nos_service_info){.kind = s->kind,
                                                .state = s->state,
                                                .users = s->device.users,
                                                .resources_owned = s->device.lease_count};
            for (size_t n = 0; s->device.name[n] && n < NOS_SERVICE_NAME_MAX; n++)
                info[i].name[n] = s->device.name[n];
        }
    }
    d->lock.leave(d->lock.context);
    return rc;
}
