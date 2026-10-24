/* Copyright (C) 2026 NOS contributors. SDK URB fault-model declarations. */
#ifndef NOS_HOST_S3_USB_SDK_H
#define NOS_HOST_S3_USB_SDK_H
#include <stdbool.h>
#include <stdint.h>
#include "nos/driver/esp_idf.h"
#include "nos/driver/usb_service.h"
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NOT_FINISHED 0x10c
#define ESP_INTR_FLAG_LEVEL1 1
#define USB_SPEED_LOW 0
#define USB_SPEED_FULL 1
#define USB_HOST_CLIENT_EVENT_NEW_DEV 1
#define USB_HOST_CLIENT_EVENT_DEV_GONE 2
#define USB_TRANSFER_STATUS_COMPLETED 0
#define USB_TRANSFER_STATUS_STALL 1
#define USB_TRANSFER_STATUS_CANCELED 2
#define USB_TRANSFER_STATUS_NO_DEVICE 3
typedef unsigned TickType_t;
typedef void *TaskHandle_t;
typedef struct {
    unsigned rx_buffer_size, tx_buffer_size;
} usb_serial_jtag_driver_config_t;
int usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *);
int usb_serial_jtag_driver_uninstall(void);
int usb_serial_jtag_read_bytes(void *, uint32_t, TickType_t);
int usb_serial_jtag_write_bytes(const void *, size_t, TickType_t);
int usb_serial_jtag_wait_tx_done(TickType_t);
extern const struct nos_service_ops idf_usb_serial_ops;
#define pdPASS 1
#define pdMS_TO_TICKS(ms) ((ms) / 10)
#define GPIO_MODE_INPUT 1
#define GPIO_MODE_OUTPUT 2
typedef struct {
    uint64_t pin_bit_mask;
    int mode;
    int pull_up_en, pull_down_en;
} gpio_config_t;
typedef void *usb_host_client_handle_t;
typedef void *usb_device_handle_t;
typedef struct usb_transfer_s {
    uint8_t *data_buffer;
    size_t data_buffer_size;
    int num_bytes, actual_num_bytes, status;
    usb_device_handle_t device_handle;
    uint8_t bEndpointAddress;
    void (*callback)(struct usb_transfer_s *);
    void *context;
} usb_transfer_t;
typedef struct {
    uint8_t bMaxPacketSize0;
    uint16_t idVendor, idProduct;
} usb_device_desc_t;
typedef struct __attribute__((packed)) {
    uint8_t length, type;
    uint16_t wTotalLength;
    uint8_t tail[5];
} usb_config_desc_t;
typedef struct {
    int speed;
} usb_device_info_t;
typedef struct {
    int event;
    union {
        struct {
            uint8_t address;
        } new_dev;
        struct {
            void *dev_hdl;
        } dev_gone;
    };
} usb_host_client_event_msg_t;
typedef struct {
    int intr_flags;
} usb_host_config_t;
typedef struct {
    int is_synchronous, max_num_event_msg;
    struct {
        void (*client_event_callback)(const usb_host_client_event_msg_t *, void *);
        void *callback_arg;
    } async;
} usb_host_client_config_t;
struct idf_device {
    struct nos_service service;
    struct nos_idf_device_config config;
    uint8_t installed;
    void *otg;
    union {
        void *usb;
    } u;
};
int idf_error(int);
int64_t esp_timer_get_time(void);
void vTaskDelay(unsigned);
int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *);
void vTaskDelete(TaskHandle_t);
int gpio_set_level(int, int);
int gpio_set_direction(int, int);
int gpio_get_level(int);
int gpio_config(const gpio_config_t *);
int usb_host_install(const usb_host_config_t *);
int usb_host_uninstall(void);
int usb_host_client_register(const usb_host_client_config_t *, void **);
int usb_host_client_deregister(void *);
int usb_host_client_handle_events(void *, unsigned);
int usb_host_lib_handle_events(unsigned, uint32_t *);
int usb_host_lib_set_root_port_power(bool);
int usb_host_device_open(void *, uint8_t, void **);
int usb_host_device_close(void *, void *);
int usb_host_device_free_all(void);
int usb_host_device_info(void *, usb_device_info_t *);
int usb_host_get_device_descriptor(void *, const usb_device_desc_t **);
int usb_host_get_active_config_descriptor(void *, const usb_config_desc_t **);
int usb_host_interface_claim(void *, void *, uint8_t, uint8_t);
int usb_host_interface_release(void *, void *, uint8_t);
int usb_host_endpoint_halt(void *, uint8_t);
int usb_host_endpoint_flush(void *, uint8_t);
int usb_host_endpoint_clear(void *, uint8_t);
int usb_host_transfer_alloc(size_t, int, usb_transfer_t **);
int usb_host_transfer_free(usb_transfer_t *);
int usb_host_transfer_submit(usb_transfer_t *);
int usb_host_transfer_submit_control(void *, usb_transfer_t *);
extern const struct nos_service_ops idf_usb_host_ops;
extern const struct nos_service_ops idf_usb_device_ops, idf_usb_otg_ops;
#endif
