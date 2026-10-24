/* Copyright (C) 2026 NOS contributors. Actual Wi-Fi adapter, injected SDK. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "host_esp32s3_wifi_sdk.h"
enum fault { NONE, ALLOC, ATTACH, HANDLERS, INIT, STORAGE, DEINIT, CLEAR, STOP };
static enum fault fault;
static unsigned nvs_users, netifs, attached, initialized, started, associated;
static esp_netif_t netif;
int idf_error(int error)
{
    return error == ESP_OK                  ? 0
           : error == ESP_ERR_NO_MEM        ? -NOS_DRV_ENOSPC
           : error == ESP_ERR_INVALID_ARG   ? -NOS_DRV_EINVAL
           : error == ESP_ERR_INVALID_STATE ? -NOS_DRV_EBUSY
                                            : -NOS_DRV_EIO;
}
int idf_nvs_acquire(void)
{
    nvs_users++;
    return 0;
}
int idf_nvs_release(void)
{
    assert(nvs_users);
    nvs_users--;
    return 0;
}
int esp_netif_init(void)
{
    return 0;
}
int esp_event_loop_create_default(void)
{
    return 0;
}
esp_netif_t *esp_netif_new(const esp_netif_config_t *c)
{
    assert(c->marker == 1);
    if (fault == ALLOC)
        return NULL;
    netifs++;
    return &netif;
}
int esp_netif_attach_wifi_station(esp_netif_t *p)
{
    assert(p == &netif && netifs);
    attached = 1; /* Simulate IDF's pointer publication before error. */
    return fault == ATTACH ? ESP_FAIL : 0;
}
int esp_wifi_set_default_wifi_sta_handlers(void)
{
    return fault == HANDLERS ? ESP_FAIL : 0;
}
int esp_wifi_clear_default_wifi_driver_and_handlers(void *p)
{
    assert(p == &netif && attached);
    if (fault == CLEAR)
        return ESP_FAIL;
    attached = 0;
    return 0;
}
void esp_netif_destroy(esp_netif_t *p)
{
    assert(p == &netif && netifs && !attached);
    netifs--;
}
int esp_wifi_init(const wifi_init_config_t *c)
{
    assert(c->marker == 1 && !initialized);
    initialized = 1; /* Model a failed internal rollback as well as init. */
    return fault == INIT ? ESP_FAIL : 0;
}
int esp_wifi_deinit(void)
{
    assert(initialized && !started);
    if (fault == DEINIT)
        return ESP_FAIL;
    initialized = 0;
    return 0;
}
int esp_wifi_set_storage(int value)
{
    assert(value == WIFI_STORAGE_RAM);
    return fault == STORAGE ? ESP_FAIL : 0;
}
int esp_wifi_set_mode(int value)
{
    assert(value == WIFI_MODE_STA);
    return 0;
}
int esp_wifi_set_config(int index, wifi_config_t *c)
{
    assert(index == WIFI_IF_STA && c->sta.ssid[0] && !started);
    return 0;
}
int esp_wifi_start(void)
{
    assert(initialized);
    started = 1;
    return 0;
}
int esp_wifi_stop(void)
{
    assert(started);
    if (fault == STOP)
        return ESP_ERR_INVALID_STATE;
    started = associated = 0;
    return 0;
}
int esp_wifi_connect(void)
{
    assert(started);
    associated = 1;
    return 0;
}
int esp_wifi_disconnect(void)
{
    associated = 0;
    return 0;
}
int esp_wifi_sta_get_ap_info(wifi_ap_record_t *p)
{
    if (!associated)
        return ESP_ERR_WIFI_NOT_CONNECT;
    p->rssi = -42;
    return 0;
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
static void check(enum fault requested)
{
    /* Discarding a quarantined fixture models a reboot, never a live release. */
    nvs_users = netifs = attached = initialized = started = associated = 0;
    fault = requested;
    struct nos_service_domain domain;
    struct nos_device *entries[2];
    struct nos_service_lock sync = {NULL, lock, unlock};
    assert(!nos_service_domain_init(&domain, entries, 2, &sync));
    struct idf_device d = {0}, other = {0};
    struct nos_resource resource = {.id = 600, .type = NOS_RESOURCE_CONTROLLER};
    assert(!nos_service_register(&domain, &d.service, "wifi", NOS_SERVICE_WIFI, &resource, 1,
                                 &idf_wifi_ops, &d));
    assert(!nos_service_register(&domain, &other.service, "other", NOS_SERVICE_WIFI, &resource, 1,
                                 &idf_wifi_ops, &other));
    int result = nos_service_start(&d.service);
    if (requested == ALLOC || requested == HANDLERS || requested == STORAGE) {
        assert(result == (requested == ALLOC ? -NOS_DRV_ENOSPC : -NOS_DRV_EIO));
        assert(d.service.state == NOS_SERVICE_STOPPED && !d.service.device.lease_count);
        assert(!nvs_users && !netifs && !attached && !initialized);
    } else if (requested == ATTACH || requested == INIT) {
        assert(result == -NOS_DRV_EIO);
        assert(d.service.state == NOS_SERVICE_FAULTED && d.service.device.lease_count == 1);
        assert(nvs_users == 1 && netifs == 1 && attached);
        assert(nos_service_start(&other.service) == -NOS_DRV_EBUSY);
        assert(nos_service_stop(&d.service) == -NOS_DRV_EIO);
        assert(nos_service_unregister(&d.service) == -NOS_DRV_EBUSY);
        return;
    } else {
        assert(!result && d.service.state == NOS_SERVICE_RUNNING);
        struct nos_io_wifi_config config = {.ssid = "nos-test", .password = "12345678"};
        assert(!nos_service_call(&d.service, NOS_IO_CONFIGURE, &config, sizeof(config)));
        assert(!nos_service_call(&d.service, NOS_IO_CONNECT, NULL, 0));
        struct nos_io_wifi_status status;
        assert(!nos_service_call(&d.service, NOS_IO_STATUS, &status, sizeof(status)));
        assert(status.connected && status.rssi == -42);
        assert(nos_service_call(&d.service, NOS_IO_CONFIGURE, &config, sizeof(config)) ==
               -NOS_DRV_EBUSY);
        result = nos_service_stop(&d.service);
        if (requested == DEINIT || requested == CLEAR) {
            assert(result == -NOS_DRV_EIO && d.service.state == NOS_SERVICE_FAULTED);
            assert(nvs_users && netifs && d.service.device.lease_count == 1);
            assert(nos_service_start(&other.service) == -NOS_DRV_EBUSY);
            fault = NONE;
            assert(nos_service_stop(&d.service) == -NOS_DRV_EIO); /* Requires reboot. */
            return;
        }
        if (requested == STOP) {
            assert(result == -NOS_DRV_EBUSY && d.service.state == NOS_SERVICE_FAULTED);
            assert(nos_service_start(&other.service) == -NOS_DRV_EBUSY);
            fault = NONE;
            assert(!nos_service_stop(&d.service));
        } else
            assert(!result);
        assert(!nvs_users && !netifs && !attached && !initialized && !started);
    }
    assert(!nos_service_unregister(&d.service));
    assert(!nos_service_unregister(&other.service));
}
int main(void)
{
    for (unsigned i = NONE; i <= STOP; i++)
        check(i);
    puts("PASS idf_wifi_actual_adapter_allocation_rollback_quarantine_stop_retry");
}
