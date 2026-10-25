/* Copyright (C) 2026 NOS contributors. Backend-private SDK handles. */
#ifndef NOS_IDF_DEVICES_PRIVATE_H
#define NOS_IDF_DEVICES_PRIVATE_H
#include "nos/driver/esp_idf.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "nvs.h"
#include "esp_ota_ops.h"
#include "soc/soc_caps.h"
#include "flash_block.h"
struct idf_device {
    struct nos_service service;
    struct nos_idf_device_config config;
    const struct nos_service_ops *backend;
    void *otg;
    struct nos_resource resources[NOS_DRIVER_MAX_RESOURCES];
    uint8_t installed, faulted, nvs_owned;
    union {
        struct nos_flash_block block;
        struct {
            struct nos_gpio_controller controller;
            struct nos_gpio pin;
        } gpio;
        struct nos_uart uart;
        struct {
            struct nos_spi_bus bus;
            spi_device_handle_t handle;
            spi_transaction_t transaction;
            uint8_t tx[64], rx[64], polling;
        } spi;
        struct {
            struct nos_i2c_bus bus;
            i2c_master_bus_handle_t handle;
            i2c_master_dev_handle_t device;
        } i2c;
        struct {
            nvs_handle_t handle;
        } kv;
        struct {
            esp_ota_handle_t handle;
            uint8_t begun;
        } ota;
        struct {
            void *netif;
            uint8_t started, configured, attached;
        } wifi;
        struct {
            void *gatt;
            TaskHandle_t task, stop_task;
            SemaphoreHandle_t done, stop_done;
            int stop_result;
            uint8_t stopped, host_signaled, stop_signaled;
        } ble;
        void *usb;
        void *auxiliary;
    } u;
};
int idf_error(int);
int idf_nvs_acquire(void);
int idf_nvs_release(void);
extern const struct nos_service_ops idf_gpio_ops, idf_uart_ops, idf_spi_ops, idf_i2c_ops;
extern const struct nos_service_ops idf_storage_ops, idf_ota_ops;
extern const struct nos_service_ops idf_console_ops;
extern const struct nos_service_ops idf_flash_ops;
#ifdef NOS_IDF_ECOSYSTEM
extern const struct nos_service_ops idf_wifi_ops, idf_ble_ops, idf_usb_host_ops, idf_usb_device_ops;
extern const struct nos_service_ops idf_auxiliary_ops;
extern const struct nos_service_ops idf_can_ops;
extern const struct nos_service_ops idf_usb_otg_ops, idf_usb_serial_ops;
#endif
#endif
