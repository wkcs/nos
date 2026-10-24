/* Copyright (C) 2026 NOS contributors. Fault injection against real manager. */
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include "driver/service.h"
static pthread_mutex_t mutex;
static int lock(void *p)
{
    return pthread_mutex_lock(p) ? -NOS_DRV_EBUSY : 0;
}
static void unlock(void *p)
{
    assert(!pthread_mutex_unlock(p));
}
struct backend {
    struct nos_service *service;
    int start_error, stop_error, calls, stops;
};
static int start(void *p)
{
    return ((struct backend *)p)->start_error;
}
static int stop(void *p)
{
    struct backend *b = p;
    b->stops++;
    return b->stop_error;
}
static int call(void *p, uint32_t command, void *request, size_t size)
{
    struct backend *b = p;
    (void)request;
    (void)size;
    b->calls++;
    assert(nos_service_stop(b->service) == -NOS_DRV_EBUSY);
    assert(nos_service_call(b->service, 0, NULL, 0) == -NOS_DRV_EBUSY);
    return command == 99 ? 1 : 0; /* Positive backend statuses must fail. */
}
static const struct nos_service_ops ops = {start, stop, call};
static void *worker(void *p)
{
    for (unsigned i = 0; i < 1000; i++)
        assert(nos_service_call(p, 0, NULL, 0) == 0);
    return NULL;
}
int main(void)
{
    pthread_mutexattr_t attr;
    assert(!pthread_mutexattr_init(&attr));
    assert(!pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE));
    assert(!pthread_mutex_init(&mutex, &attr));
    assert(!pthread_mutexattr_destroy(&attr));
    struct nos_service_domain domain;
    struct nos_device *entries[4];
    struct nos_service_lock sync = {&mutex, lock, unlock};
    assert(!nos_service_domain_init(&domain, entries, 4, &sync));
    struct nos_service a = {0}, b = {0};
    struct backend ba = {.service = &a}, bb = {.service = &b};
    struct nos_resource ra[] = {{.id = 100, .type = NOS_RESOURCE_CONTROLLER},
                                {.id = 4, .type = NOS_RESOURCE_PINCTRL}};
    struct nos_resource rb[] = {{.id = 101, .type = NOS_RESOURCE_CONTROLLER},
                                {.id = 4, .type = NOS_RESOURCE_PINCTRL}};
    assert(!nos_service_register(&domain, &a, "a", NOS_SERVICE_SPI, ra, 2, &ops, &ba));
    assert(!nos_service_register(&domain, &b, "b", NOS_SERVICE_I2C, rb, 2, &ops, &bb));
    assert(nos_service_call(&a, 0, NULL, 0) == -NOS_DRV_ENODEV);
    assert(!nos_service_start(&a));
    struct nos_service_info snapshot[2];
    size_t count;
    assert(nos_service_snapshot(&domain, NULL, 0, &count) == -NOS_DRV_ENOSPC && count == 2);
    assert(!nos_service_snapshot(&domain, snapshot, 2, &count));
    assert(snapshot[0].state == NOS_SERVICE_RUNNING && snapshot[0].resources_owned == 2);
    assert(nos_service_start(&b) == -NOS_DRV_EBUSY && bb.stops == 0);
    /* A partial acquisition before conflicting pin4 was rolled back. */
    for (unsigned i = 0; i < NOS_SERVICE_CLAIMS; i++)
        assert(!domain.claims[i].owned || domain.claims[i].id != 101);
    assert(nos_service_unregister(&a) == -NOS_DRV_EBUSY);
    assert(nos_device_unbind(&a.device) == -NOS_DRV_EBUSY);
    assert(nos_service_call(&a, 99, NULL, 0) == -NOS_DRV_EIO);
    assert(nos_service_call(&a, 0, NULL, 1) == -NOS_DRV_EINVAL);
    pthread_t t[4];
    for (unsigned i = 0; i < 4; i++)
        assert(!pthread_create(&t[i], NULL, worker, &a));
    for (unsigned i = 0; i < 4; i++)
        assert(!pthread_join(t[i], NULL));
    assert(ba.calls == 4001);
    ba.stop_error = -NOS_DRV_EIO;
    assert(nos_service_stop(&a) == -NOS_DRV_EIO && a.state == NOS_SERVICE_FAULTED);
    assert(nos_service_call(&a, 0, NULL, 0) == -NOS_DRV_ENODEV);
    assert(nos_service_start(&b) == -NOS_DRV_EBUSY);
    ba.stop_error = 0;
    assert(!nos_service_stop(&a));
    ba.start_error = -NOS_DRV_ENOSPC;
    assert(nos_service_start(&a) == -NOS_DRV_ENOSPC && a.state == NOS_SERVICE_STOPPED);
    assert(!nos_service_start(&b));
    assert(!nos_service_stop(&b));
    /* Failed start plus failed rollback also keeps claims, without becoming
     * callable. Retrying stop is the only path to release the hardware. */
    ba.stop_error = -NOS_DRV_EIO;
    assert(nos_service_start(&a) == -NOS_DRV_ENOSPC && a.state == NOS_SERVICE_FAULTED);
    assert(nos_service_start(&b) == -NOS_DRV_EBUSY);
    assert(nos_service_unregister(&a) == -NOS_DRV_EBUSY);
    ba.stop_error = 0;
    assert(!nos_service_stop(&a));
    assert(!nos_service_unregister(&a) && !nos_service_unregister(&b));
    assert(!nos_service_snapshot(&domain, NULL, 0, &count) && !count);
    assert(!domain.registry.count);
    for (unsigned i = 0; i < NOS_SERVICE_CLAIMS; i++)
        assert(!domain.claims[i].owned);
    struct nos_device *capacity_entries[9];
    struct nos_service capacity[9] = {0};
    struct backend backends[9] = {0};
    struct nos_resource resources[9][16];
    char names[9][8];
    assert(!nos_service_domain_init(&domain, capacity_entries, 9, &sync));
    for (unsigned i = 0; i < 9; i++) {
        snprintf(names[i], sizeof(names[i]), "limit%u", i);
        backends[i].service = &capacity[i];
        for (unsigned j = 0; j < 16; j++)
            resources[i][j] =
                (struct nos_resource){.id = 1000 + i * 16 + j, .type = NOS_RESOURCE_CONTROLLER};
        assert(!nos_service_register(&domain, &capacity[i], names[i], NOS_SERVICE_SPI, resources[i],
                                     16, &ops, &backends[i]));
        assert(nos_service_start(&capacity[i]) == (i == 8 ? -NOS_DRV_ENOSPC : 0));
    }
    assert(!nos_service_stop(&capacity[0]));
    assert(!nos_service_start(&capacity[8]));
    for (unsigned i = 0; i < 9; i++) {
        if (i)
            assert(!nos_service_stop(&capacity[i]));
        assert(!nos_service_unregister(&capacity[i]));
    }
    for (unsigned i = 0; i < NOS_SERVICE_CLAIMS; i++)
        assert(!domain.claims[i].owned);
    assert(!pthread_mutex_destroy(&mutex));
    puts("PASS managed_devices_conflicts_rollback_quarantine_retry_reentry_threads");
}
