/* Copyright (C) 2026 NOS contributors. NOS owns lifecycle; IDF owns hardware. */
#include <stdlib.h>
#include "private.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "nos/arch.h"
static struct nos_service_domain domain;
static struct nos_device *entries[32];
static SemaphoreHandle_t mutex;
static StaticSemaphore_t mutex_storage;
static portMUX_TYPE init_lock = portMUX_INITIALIZER_UNLOCKED;
static unsigned initialized;
static struct nos_service reserved;
static struct nos_service console;
#if CONFIG_IDF_TARGET_ESP32C3
static const struct nos_resource console_resources[] = {
    {.id = NOS_SERVICE_UART << 16, .type = NOS_RESOURCE_CONTROLLER},
    {.id = 20, .type = NOS_RESOURCE_PINCTRL}, {.id = 21, .type = NOS_RESOURCE_PINCTRL}};
#else
static const struct nos_resource console_resources[] = {
    {.id = NOS_SERVICE_UART << 16, .type = NOS_RESOURCE_CONTROLLER},
    {.id = 43, .type = NOS_RESOURCE_PINCTRL}, {.id = 44, .type = NOS_RESOURCE_PINCTRL}};
#endif
#if CONFIG_IDF_TARGET_ESP32C3
static const uint8_t system_pins[] = {11, 12, 13, 14, 15, 16, 17};
#else
static const uint8_t system_pins[] = {26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37};
#endif
static struct nos_resource reserved_pins[sizeof(system_pins)];
static int lock(void *context)
{
    if (!arch_syscall_ready())
        return -NOS_DRV_EBUSY;
    return xSemaphoreTakeRecursive(context, portMAX_DELAY) ? 0 : -NOS_DRV_EBUSY;
}
static void unlock(void *context)
{
    xSemaphoreGiveRecursive(context);
}
int idf_error(int error)
{
    switch (error) {
    case ESP_OK:
        return 0;
    case ESP_ERR_INVALID_ARG:
        return -NOS_DRV_EINVAL;
    case ESP_ERR_NO_MEM:
        return -NOS_DRV_ENOSPC;
    case ESP_ERR_TIMEOUT:
        return -NOS_DRV_ETIMEDOUT;
    case ESP_ERR_NOT_SUPPORTED:
        return -NOS_DRV_ENOTSUP;
    case ESP_ERR_INVALID_STATE:
        return -NOS_DRV_EBUSY;
    case ESP_ERR_NOT_FOUND:
        return -NOS_DRV_ENODEV;
    default:
        return -NOS_DRV_EIO;
    }
}
static int no_start(void *p)
{
    (void)p;
    return 0;
}
static const struct nos_service_ops reserve_ops = {no_start, no_start, NULL};
int nos_idf_devices_init(void)
{
    if (!arch_syscall_ready())
        return -NOS_DRV_EBUSY;
    /* Creation uses static storage only. Publish the mutex under the CPU lock;
     * all slow registration work runs under the recursive domain mutex. */
    portENTER_CRITICAL(&init_lock);
    if (!mutex)
        mutex = xSemaphoreCreateRecursiveMutexStatic(&mutex_storage);
    portEXIT_CRITICAL(&init_lock);
    if (!mutex)
        return -NOS_DRV_ENOSPC;
    int rc = lock(mutex);
    if (rc)
        return rc;
    if (!initialized) {
        struct nos_service_lock sync = {mutex, lock, unlock};
        rc = nos_service_domain_init(&domain, entries, 32, &sync);
        for (unsigned i = 0; i < sizeof(system_pins); i++)
            reserved_pins[i] = (struct nos_resource){.id = system_pins[i],
                                                     .type = NOS_RESOURCE_PINCTRL};
        if (!rc)
            rc = nos_service_register(&domain, &reserved, "system.flash-psram",
                                      NOS_SERVICE_STORAGE, reserved_pins, sizeof(system_pins), &reserve_ops, NULL);
        if (!rc)
            rc = nos_service_start(&reserved);
        if (!rc)
            rc = nos_service_register(&domain, &console, "system.console", NOS_SERVICE_CONSOLE,
                                      console_resources, 3, &idf_console_ops, &console);
        if (!rc)
            rc = nos_service_start(&console);
        initialized = rc ? 2 : 1;
    }
    if (initialized != 1 && !rc)
        rc = -NOS_DRV_EIO;
    unlock(mutex);
    return rc;
}
int nos_idf_console_call(uint32_t command, void *request, size_t size)
{
    return nos_service_call(&console, command, request, size);
}
static const struct nos_service_ops *backend(enum nos_service_kind kind)
{
    switch (kind) {
    case NOS_SERVICE_GPIO:
        return &idf_gpio_ops;
    case NOS_SERVICE_UART:
        return &idf_uart_ops;
    case NOS_SERVICE_SPI:
        return &idf_spi_ops;
    case NOS_SERVICE_I2C:
        return &idf_i2c_ops;
    case NOS_SERVICE_STORAGE:
        return &idf_storage_ops;
    case NOS_SERVICE_OTA:
        return &idf_ota_ops;
#ifdef NOS_IDF_ECOSYSTEM
    case NOS_SERVICE_WIFI:
        return &idf_wifi_ops;
    case NOS_SERVICE_BLE:
        return &idf_ble_ops;
#if SOC_USB_OTG_SUPPORTED
    case NOS_SERVICE_USB_HOST:
        return &idf_usb_host_ops;
    case NOS_SERVICE_USB_DEVICE:
        return &idf_usb_device_ops;
    case NOS_SERVICE_USB_OTG:
        return &idf_usb_otg_ops;
#endif
    case NOS_SERVICE_USB_SERIAL_JTAG:
        return &idf_usb_serial_ops;
    case NOS_SERVICE_CAN:
        return &idf_can_ops;
    case NOS_SERVICE_I2S:
    case NOS_SERVICE_PWM:
    case NOS_SERVICE_RMT:
    case NOS_SERVICE_TIMER:
#if SOC_MCPWM_SUPPORTED
    case NOS_SERVICE_MCPWM:
#endif
#if SOC_PCNT_SUPPORTED
    case NOS_SERVICE_COUNTER:
#endif
    case NOS_SERVICE_ADC:
    case NOS_SERVICE_TEMPERATURE:
#if SOC_SDMMC_HOST_SUPPORTED
    case NOS_SERVICE_SDMMC:
#endif
    case NOS_SERVICE_SDSPI:
#if SOC_LCD_RGB_SUPPORTED
    case NOS_SERVICE_DISPLAY:
#endif
        return &idf_auxiliary_ops;
#endif
    default:
        return NULL;
    }
}
static int managed_start(void *context)
{
    struct idf_device *d = context;
    return d->backend->start(d);
}
static int managed_stop(void *context)
{
    struct idf_device *d = context;
    int rc = d->backend->stop(d);
    if (rc)
        return rc;
    /* Disconnect pinmux only AFTER SDK IRQ/DMA and handles are retired. A
     * failed reset also keeps the NOS claims, and may be retried safely. */
    for (unsigned i = 0; i < d->service.device.resource_count; i++)
        if (d->resources[i].type == NOS_RESOURCE_PINCTRL) {
            rc = idf_error(gpio_reset_pin(d->resources[i].id));
            if (rc)
                return rc;
        }
    return 0;
}
static int managed_call(void *context, uint32_t command, void *request, size_t size)
{
    struct idf_device *d = context;
    return d->backend->call ? d->backend->call(d, command, request, size) : -NOS_DRV_ENOTSUP;
}
static const struct nos_service_ops managed_ops = {managed_start, managed_stop, managed_call};
int nos_idf_device_create(const struct nos_idf_device_config *config, struct nos_service **out)
{
    unsigned pins = 0;
    int rc;
    if (!out)
        return -NOS_DRV_EINVAL;
    *out = NULL;
    if (!config || !config->name)
        return -NOS_DRV_EINVAL;
    const struct nos_service_ops *ops = backend(config->kind);
    if (!ops)
        return -NOS_DRV_ENOTSUP;
    if (config->kind == NOS_SERVICE_USB_OTG) {
        const struct nos_idf_usb_otg_config *o = config->usb_otg;
        if (!o || !o->allowed || (o->allowed & ~3u) || o->source_allowed > 1 ||
            o->id_active_low > 1 || o->vbus_active_low > 1 || o->enable_active_low > 1 ||
            o->overcurrent_active_low > 1 || o->vbus_sense_pin < 0 ||
            ((o->allowed & NOS_USB_ALLOW_HOST) && o->vbus_enable_pin < 0) || o->poll_ms > 1000 ||
            o->debounce_ms > 1000)
            return -NOS_DRV_EINVAL;
        int pins[] = {o->id_pin, o->vbus_sense_pin, o->vbus_enable_pin, o->overcurrent_pin};
        for (unsigned i = 0; i < 4; i++) {
            if (pins[i] < -1 ||
                (pins[i] >= 0 && (!GPIO_IS_VALID_GPIO(pins[i]) || pins[i] == 19 || pins[i] == 20)))
                return -NOS_DRV_EINVAL;
            for (unsigned j = 0; pins[i] >= 0 && j < i; j++)
                if (pins[i] == pins[j])
                    return -NOS_DRV_EINVAL;
        }
        if (o->vbus_enable_pin >= 0 && !GPIO_IS_VALID_OUTPUT_GPIO(o->vbus_enable_pin))
            return -NOS_DRV_EINVAL;
    }
    switch (config->kind) {
    case NOS_SERVICE_GPIO:
        pins = 1;
        break;
    case NOS_SERVICE_UART:
        if (config->instance < 1 || config->instance >= SOC_UART_NUM)
            return -NOS_DRV_EINVAL;
        pins = 4;
        break;
    case NOS_SERVICE_SPI:
        if (config->instance < 2 || config->instance > SOC_SPI_PERIPH_NUM || !config->hz || config->hz > 40000000)
            return -NOS_DRV_EINVAL;
        pins = 4;
        break;
    case NOS_SERVICE_I2C:
        if (config->instance >= SOC_I2C_NUM || !config->hz || config->hz > 400000)
            return -NOS_DRV_EINVAL;
        pins = 2;
        break;
    case NOS_SERVICE_I2S:
        if (config->instance >= SOC_I2S_NUM || config->hz < 8000 || config->hz > 96000)
            return -NOS_DRV_EINVAL;
        pins = 4;
        break;
    case NOS_SERVICE_PWM:
        if (config->instance > 3 || !config->hz || config->hz > 20000)
            return -NOS_DRV_EINVAL;
        pins = 1;
        break;
    case NOS_SERVICE_RMT:
        if (config->instance >= SOC_RMT_TX_CANDIDATES_PER_GROUP || config->hz < 1000000 || config->hz > 20000000)
            return -NOS_DRV_EINVAL;
        pins = 1;
        break;
    case NOS_SERVICE_TIMER:
        if (config->instance >= SOC_TIMER_GROUP_TOTAL_TIMERS || !config->hz || config->hz > 10000000)
            return -NOS_DRV_EINVAL;
        break;
    case NOS_SERVICE_MCPWM:
        if (config->instance > 1 || !config->hz || config->hz > 20000)
            return -NOS_DRV_EINVAL;
        pins = 1;
        break;
    case NOS_SERVICE_COUNTER:
        if (config->instance > 3)
            return -NOS_DRV_EINVAL;
        pins = 2;
        break;
    case NOS_SERVICE_ADC:
        if (config->instance < 1 || config->instance > 2)
            return -NOS_DRV_EINVAL;
        pins = 1;
        break;
    case NOS_SERVICE_SDMMC:
        if (config->instance != 1)
            return -NOS_DRV_EINVAL;
        pins = 3;
        break;
    case NOS_SERVICE_SDSPI:
        if (config->instance < 2 || config->instance > SOC_SPI_PERIPH_NUM)
            return -NOS_DRV_EINVAL;
        pins = 4;
        break;
    case NOS_SERVICE_DISPLAY:
        if (config->instance || !config->width || !config->height || config->width > 1024 ||
            config->height > 1024 || !config->hz || !config->hsync || !config->vsync)
            return -NOS_DRV_EINVAL;
        pins = 12;
        break;
    case NOS_SERVICE_CAN:
        if (config->instance || (config->hz != 125000 && config->hz != 250000 &&
                                 config->hz != 500000 && config->hz != 1000000))
            return -NOS_DRV_EINVAL;
        pins = 2;
        break;
    default:
        if (config->instance)
            return -NOS_DRV_EINVAL;
        break;
    }
    for (unsigned i = 0; i < pins; i++) {
        int pin = config->pins[i];
        if (pin < -1 || (pin >= 0 && !GPIO_IS_VALID_GPIO(pin)))
            return -NOS_DRV_EINVAL;
        if (pin < 0 &&
            (pins == 1 || config->kind == NOS_SERVICE_I2C || config->kind == NOS_SERVICE_CAN ||
             config->kind == NOS_SERVICE_SDSPI || config->kind == NOS_SERVICE_SDMMC ||
             config->kind == NOS_SERVICE_DISPLAY || (config->kind == NOS_SERVICE_I2S && i < 3) ||
             (config->kind == NOS_SERVICE_SPI && (i == 0 || i == 3))))
            return -NOS_DRV_EINVAL;
        for (unsigned j = 0; pin >= 0 && j < i; j++)
            if (pin == config->pins[j])
                return -NOS_DRV_EINVAL;
    }
    if ((rc = nos_idf_devices_init()))
        return rc;
    struct idf_device *d = calloc(1, sizeof(*d));
    if (!d)
        return -NOS_DRV_ENOSPC;
    d->config = *config;
    d->backend = ops;
    /* Host/Device alias the SAME physical USB controller and GPIO19/20. */
    unsigned identity =
        config->kind == NOS_SERVICE_USB_DEVICE || config->kind == NOS_SERVICE_USB_OTG ||
        config->kind == NOS_SERVICE_USB_SERIAL_JTAG
            ? NOS_SERVICE_USB_HOST
            : config->kind;
    if (config->kind == NOS_SERVICE_SDSPI)
        identity = NOS_SERVICE_SPI;
    d->resources[0] = (struct nos_resource){.id = (identity << 16) | config->instance,
                                            .type = NOS_RESOURCE_CONTROLLER};
    unsigned count = 1;
    for (unsigned i = 0; i < pins; i++)
        if (config->pins[i] >= 0)
            d->resources[count++] =
                (struct nos_resource){.id = config->pins[i], .type = NOS_RESOURCE_PINCTRL};
    if (config->kind == NOS_SERVICE_USB_HOST || config->kind == NOS_SERVICE_USB_DEVICE ||
        config->kind == NOS_SERVICE_USB_OTG || config->kind == NOS_SERVICE_USB_SERIAL_JTAG) {
#if CONFIG_IDF_TARGET_ESP32C3
        d->resources[count++] = (struct nos_resource){.id = 18, .type = NOS_RESOURCE_PINCTRL};
        d->resources[count++] = (struct nos_resource){.id = 19, .type = NOS_RESOURCE_PINCTRL};
#else
        d->resources[count++] = (struct nos_resource){.id = 19, .type = NOS_RESOURCE_PINCTRL};
        d->resources[count++] = (struct nos_resource){.id = 20, .type = NOS_RESOURCE_PINCTRL};
#endif
    }
    if (config->kind == NOS_SERVICE_USB_OTG) {
        const struct nos_idf_usb_otg_config *o = config->usb_otg;
        int pins[] = {o->id_pin, o->vbus_sense_pin, o->vbus_enable_pin, o->overcurrent_pin};
        for (unsigned i = 0; i < 4; i++)
            if (pins[i] >= 0)
                d->resources[count++] =
                    (struct nos_resource){.id = pins[i], .type = NOS_RESOURCE_PINCTRL};
    }
    /* A GPIO service identity is its pin, allowing independent pins. */
    if (config->kind == NOS_SERVICE_GPIO)
        d->resources[0].id = config->pins[0];
    rc = nos_service_register(&domain, &d->service, config->name, config->kind, d->resources, count,
                              &managed_ops, d);
    if (rc)
        free(d);
    else
        *out = &d->service;
    return rc;
}
int nos_idf_device_destroy(struct nos_service **service)
{
    if (!service || !*service || (*service)->domain != &domain || *service == &reserved ||
        *service == &console)
        return -NOS_DRV_EINVAL;
    struct idf_device *d = (struct idf_device *)*service;
    int rc = lock(mutex);
    if (rc)
        return rc;
    if (d->service.state != NOS_SERVICE_STOPPED)
        rc = nos_service_stop(&d->service);
    if (!rc)
        rc = nos_service_unregister(&d->service);
    if (!rc) {
        free(d);
        *service = NULL;
    }
    unlock(mutex);
    return rc;
}
int nos_idf_devices_snapshot(struct nos_service_info *info, size_t capacity, size_t *count)
{
    int rc = nos_idf_devices_init();
    return rc ? rc : nos_service_snapshot(&domain, info, capacity, count);
}
