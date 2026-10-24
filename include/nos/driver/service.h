/* Copyright (C) 2026 NOS contributors. */
#ifndef NOS_DRIVER_SERVICE_H
#define NOS_DRIVER_SERVICE_H
#include "device.h"

/* Platform-neutral service boundary. SDK handles stay in backend context.
 * All lifecycle and requests run under the supplied domain lock, in task
 * context. Backends must not synchronously call back into this domain from
 * another task. An ISR must enqueue an event instead. No isolation is implied. */
enum nos_service_kind {
    NOS_SERVICE_GPIO,
    NOS_SERVICE_UART,
    NOS_SERVICE_SPI,
    NOS_SERVICE_I2C,
    NOS_SERVICE_STORAGE,
    NOS_SERVICE_OTA,
    NOS_SERVICE_WIFI,
    NOS_SERVICE_BLE,
    NOS_SERVICE_USB_HOST,
    NOS_SERVICE_USB_DEVICE,
    NOS_SERVICE_I2S,
    NOS_SERVICE_PWM,
    NOS_SERVICE_RMT,
    NOS_SERVICE_TIMER,
    NOS_SERVICE_MCPWM,
    NOS_SERVICE_COUNTER,
    NOS_SERVICE_ADC,
    NOS_SERVICE_TEMPERATURE,
    NOS_SERVICE_SDMMC,
    NOS_SERVICE_SDSPI,
    NOS_SERVICE_DISPLAY,
    NOS_SERVICE_CAN,
    NOS_SERVICE_KIND_COUNT
};
enum nos_service_state { NOS_SERVICE_STOPPED, NOS_SERVICE_RUNNING, NOS_SERVICE_FAULTED };
struct nos_service;
struct nos_service_ops {
    /* stop() MUST tolerate a partially failed start(). On failure stop keeps
     * all resource claims: callers may retry, never free/reuse the context. */
    int (*start)(void *);
    int (*stop)(void *);
    int (*call)(void *, uint32_t command, void *request, size_t size);
};
struct nos_service_lock {
    void *context;
    int (*enter)(void *);
    void (*leave)(void *);
};
#define NOS_SERVICE_CLAIMS 128u
#define NOS_SERVICE_NAME_MAX 63u
struct nos_service_info {
    char name[NOS_SERVICE_NAME_MAX + 1];
    enum nos_service_kind kind;
    enum nos_service_state state;
    uint16_t users;
    uint8_t resources_owned;
};
struct nos_service_domain {
    struct nos_device_registry registry;
    struct nos_service_lock lock;
    struct nos_resource_provider provider;
    struct {
        enum nos_resource_type type;
        uint32_t id;
        uint8_t owned;
    } claims[NOS_SERVICE_CLAIMS];
};
/* Zero-initialized, immovable until unregister succeeds. Use service APIs,
 * rather than mutating the embedded device directly. Resource descriptors
 * and name remain caller-owned for the registration lifetime. */
struct nos_service {
    struct nos_device device;
    struct nos_resource_lease leases[NOS_DRIVER_MAX_RESOURCES];
    struct nos_service_domain *domain;
    const struct nos_service_ops *ops;
    void *context;
    enum nos_service_kind kind;
    enum nos_service_state state;
    uint8_t in_call;
};
int nos_service_domain_init(struct nos_service_domain *, struct nos_device **entries,
                            size_t capacity, const struct nos_service_lock *);
int nos_service_register(struct nos_service_domain *, struct nos_service *, const char *name,
                         enum nos_service_kind, const struct nos_resource *, size_t count,
                         const struct nos_service_ops *, void *context);
int nos_service_unregister(struct nos_service *);
int nos_service_start(struct nos_service *);
int nos_service_stop(struct nos_service *);
int nos_service_call(struct nos_service *, uint32_t command, void *request, size_t size);
/* Atomic copied snapshot. ENOSPC sets required count without partial output. */
int nos_service_snapshot(struct nos_service_domain *, struct nos_service_info *, size_t capacity,
                         size_t *count);
#endif
