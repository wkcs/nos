/* Copyright (C) 2026 NOS contributors. Retain official radio/host libraries. */
#include <string.h>
#include "private.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "esp_bt.h"
static unsigned network_initialized;
static int wifi_start(void *context)
{
    struct idf_device *d = context;
    int rc = idf_nvs_acquire();
    if (rc)
        return rc;
    d->nvs_owned = 1;
    /* lwIP is runtime infrastructure, retained for the firmware lifetime;
     * IDF esp_netif_deinit itself is unsupported. Interfaces are per device. */
    if (!network_initialized) {
        rc = idf_error(esp_netif_init());
        if (rc)
            return rc;
        int result = esp_event_loop_create_default();
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
            return idf_error(result);
        network_initialized = 1;
    }
    d->u.wifi.netif = esp_netif_create_default_wifi_sta();
    if (!d->u.wifi.netif)
        return -NOS_DRV_ENOSPC;
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    rc = idf_error(esp_wifi_init(&config));
    if (rc)
        return rc;
    d->installed = 1;
    /* Credentials stay in RAM unless an explicit NOS persistence policy saves
     * them. No implicit writes or credential logging by this adapter. */
    rc = idf_error(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    if (!rc)
        rc = idf_error(esp_wifi_set_mode(WIFI_MODE_STA));
    return rc;
}
static int wifi_stop(void *context)
{
    struct idf_device *d = context;
    if (d->u.wifi.started) {
        int rc = idf_error(esp_wifi_stop());
        if (rc)
            return rc;
        d->u.wifi.started = 0;
    }
    if (d->installed) {
        int rc = idf_error(esp_wifi_deinit());
        if (rc)
            return rc;
        d->installed = 0;
    }
    if (d->u.wifi.netif) {
        esp_netif_destroy_default_wifi(d->u.wifi.netif);
        d->u.wifi.netif = NULL;
    }
    int rc = d->nvs_owned ? idf_nvs_release() : 0;
    if (!rc)
        d->nvs_owned = 0;
    return rc;
}
static int wifi_call(void *context, uint32_t op, void *request, size_t size)
{
    struct idf_device *d = context;
    if (op == NOS_IO_CONFIGURE && size == sizeof(struct nos_io_wifi_config)) {
        struct nos_io_wifi_config *c = request;
        size_t ssid = strnlen(c->ssid, sizeof(c->ssid));
        size_t password = strnlen(c->password, sizeof(c->password));
        if (!ssid || ssid > 32 || password > 64 || (password && password < 8))
            return -NOS_DRV_EINVAL;
        if (d->u.wifi.started)
            return -NOS_DRV_EBUSY;
        wifi_config_t config = {0};
        memcpy(config.sta.ssid, c->ssid, ssid);
        memcpy(config.sta.password, c->password, password);
        config.sta.threshold.authmode = password ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        int rc = idf_error(esp_wifi_set_config(WIFI_IF_STA, &config));
        /* Do not retain an extra plaintext copy on the task stack. */
        volatile uint8_t *p = (volatile uint8_t *)&config;
        for (size_t i = 0; i < sizeof(config); i++)
            p[i] = 0;
        if (!rc)
            d->u.wifi.configured = 1;
        return rc;
    }
    if (op == NOS_IO_CONNECT && !size) {
        if (!d->u.wifi.configured)
            return -NOS_DRV_EINVAL;
        if (!d->u.wifi.started) {
            int rc = idf_error(esp_wifi_start());
            if (rc)
                return rc;
            d->u.wifi.started = 1;
        }
        return idf_error(esp_wifi_connect()); /* Accepted, not associated yet. */
    }
    if (op == NOS_IO_DISCONNECT && !size)
        return idf_error(esp_wifi_disconnect());
    if (op == NOS_IO_STATUS && size == sizeof(struct nos_io_wifi_status)) {
        struct nos_io_wifi_status *status = request;
        *status = (struct nos_io_wifi_status){0};
        if (!d->u.wifi.started)
            return 0;
        wifi_ap_record_t info;
        int result = esp_wifi_sta_get_ap_info(&info);
        if (result == ESP_ERR_WIFI_NOT_CONNECT)
            return 0;
        if (result != ESP_OK)
            return idf_error(result);
        status->connected = 1;
        status->rssi = info.rssi;
        return 0;
    }
    return -NOS_DRV_ENOTSUP;
}
const struct nos_service_ops idf_wifi_ops = {wifi_start, wifi_stop, wifi_call};

static void ble_host(void *context)
{
    struct idf_device *d = context;
    nimble_port_run();
    xSemaphoreGive(d->u.ble.done);
    vTaskDelete(NULL);
}
static int ble_start(void *context)
{
    struct idf_device *d = context;
    int rc = idf_nvs_acquire();
    if (rc)
        return rc;
    d->nvs_owned = 1;
    d->u.ble.done = xSemaphoreCreateBinary();
    d->u.ble.stop_done = xSemaphoreCreateBinary();
    if (!d->u.ble.done || !d->u.ble.stop_done)
        return -NOS_DRV_ENOSPC;
    rc = idf_error(nimble_port_init());
    if (rc) {
        if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE)
            d->faulted = 1;
        return rc;
    }
    d->installed = 1;
    d->u.ble.stopped = 0;
    /* SDK helper discards xTaskCreate failure. Check NOS task exhaustion here. */
    if (xTaskCreate(ble_host, "nos-ble", CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE, d,
                    configMAX_PRIORITIES - 4, &d->u.ble.task) != pdPASS)
        return -NOS_DRV_ENOSPC;
    return 0;
}
static void ble_stop_worker(void *context)
{
    struct idf_device *d = context;
    d->u.ble.stop_result = idf_error(nimble_port_stop());
    xSemaphoreGive(d->u.ble.stop_done);
    vTaskDelete(NULL);
}
static int task_exited(TaskHandle_t task)
{
    for (unsigned i = 0; i < 100; i++) {
        if (eTaskGetState(task) == eDeleted)
            return 0;
        vTaskDelay(1);
    }
    return -NOS_DRV_ETIMEDOUT;
}
static int ble_stop(void *context)
{
    struct idf_device *d = context;
    if (d->faulted)
        return -NOS_DRV_EIO; /* A partially failed SDK deinit needs reboot. */
    if (d->u.ble.task) {
        if (!d->u.ble.stopped) {
            /* The vendor stop waits forever internally. Isolate it in a NOS
             * task, keep context alive, and bound the manager's wait. */
            if (!d->u.ble.stop_task && xTaskCreate(ble_stop_worker, "nos-ble-stop", 4096, d, 3,
                                                   &d->u.ble.stop_task) != pdPASS)
                return -NOS_DRV_ENOSPC;
            if (!d->u.ble.stop_signaled) {
                if (!xSemaphoreTake(d->u.ble.stop_done, pdMS_TO_TICKS(1000)))
                    return -NOS_DRV_ETIMEDOUT;
                d->u.ble.stop_signaled = 1;
            }
            int rc = task_exited(d->u.ble.stop_task);
            if (rc)
                return rc;
            d->u.ble.stop_task = NULL;
            d->u.ble.stop_signaled = 0;
            if (d->u.ble.stop_result)
                return d->u.ble.stop_result;
            d->u.ble.stopped = 1;
        }
        if (!d->u.ble.host_signaled) {
            if (!xSemaphoreTake(d->u.ble.done, pdMS_TO_TICKS(1000)))
                return -NOS_DRV_ETIMEDOUT;
            d->u.ble.host_signaled = 1;
        }
        int rc = task_exited(d->u.ble.task);
        if (rc)
            return rc;
        d->u.ble.task = NULL;
        d->u.ble.host_signaled = 0;
    }
    if (d->installed) {
        int rc = idf_error(nimble_port_deinit());
        if (rc) {
            d->faulted = 1;
            return rc;
        }
        d->installed = 0;
    }
    if (d->u.ble.done) {
        vSemaphoreDelete(d->u.ble.done);
        d->u.ble.done = NULL;
    }
    if (d->u.ble.stop_done) {
        vSemaphoreDelete(d->u.ble.stop_done);
        d->u.ble.stop_done = NULL;
    }
    int rc = d->nvs_owned ? idf_nvs_release() : 0;
    if (!rc)
        d->nvs_owned = 0;
    return rc;
}
static int ble_call(void *context, uint32_t op, void *request, size_t size)
{
    (void)context;
    if (op == NOS_IO_STATUS && size == sizeof(uint32_t)) {
        *(uint32_t *)request = ble_hs_synced() ? 1 : 0;
        return 0;
    }
    /* GATT/advertising profiles need their own portable protocol above this
     * lifecycle adapter; vendor host handles are deliberately not exported. */
    return -NOS_DRV_ENOTSUP;
}
const struct nos_service_ops idf_ble_ops = {ble_start, ble_stop, ble_call};
