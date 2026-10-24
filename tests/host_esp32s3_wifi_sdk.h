/* Copyright (C) 2026 NOS contributors. Fault model, never firmware headers. */
#ifndef NOS_HOST_WIFI_SDK_H
#define NOS_HOST_WIFI_SDK_H
#include <stdint.h>
#include "nos/driver/esp_idf.h"
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_WIFI_NOT_CONNECT 0x300f
#define WIFI_STORAGE_RAM 0
#define WIFI_MODE_STA 1
#define WIFI_IF_STA 0
#define WIFI_AUTH_OPEN 0
#define WIFI_AUTH_WPA2_PSK 3
typedef struct {
    int marker;
} wifi_init_config_t;
typedef struct {
    int marker;
} esp_netif_config_t;
typedef struct {
    int marker;
} esp_netif_t;
typedef struct {
    struct {
        uint8_t ssid[32], password[64];
        struct {
            int authmode;
        } threshold;
    } sta;
} wifi_config_t;
typedef struct {
    int8_t rssi;
} wifi_ap_record_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){1})
#define ESP_NETIF_DEFAULT_WIFI_STA() ((esp_netif_config_t){1})
struct idf_device {
    struct nos_service service;
    struct nos_idf_device_config config;
    uint8_t installed, faulted, nvs_owned;
    union {
        struct {
            void *netif;
            uint8_t started, configured, attached;
        } wifi;
    } u;
};
int idf_error(int);
int idf_nvs_acquire(void);
int idf_nvs_release(void);
int esp_netif_init(void);
int esp_event_loop_create_default(void);
esp_netif_t *esp_netif_new(const esp_netif_config_t *);
int esp_netif_attach_wifi_station(esp_netif_t *);
int esp_wifi_set_default_wifi_sta_handlers(void);
int esp_wifi_clear_default_wifi_driver_and_handlers(void *);
void esp_netif_destroy(esp_netif_t *);
int esp_wifi_init(const wifi_init_config_t *);
int esp_wifi_deinit(void);
int esp_wifi_set_storage(int);
int esp_wifi_set_mode(int);
int esp_wifi_set_config(int, wifi_config_t *);
int esp_wifi_start(void);
int esp_wifi_stop(void);
int esp_wifi_connect(void);
int esp_wifi_disconnect(void);
int esp_wifi_sta_get_ap_info(wifi_ap_record_t *);
extern const struct nos_service_ops idf_wifi_ops;
#endif
