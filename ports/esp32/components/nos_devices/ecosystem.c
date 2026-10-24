/* Copyright (C) 2026 NOS contributors.
 * Retain real SDK entry points and their transitive OS dependencies at link
 * time. This is a linkage audit; no RF or unconnected external bus is started. */
#include <stdint.h>
#include "soc/soc_caps.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "driver/uart.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/rmt_tx.h"
#include "driver/gptimer.h"
#if SOC_MCPWM_SUPPORTED
#include "driver/mcpwm_prelude.h"
#endif
#if SOC_PCNT_SUPPORTED
#include "driver/pulse_cnt.h"
#endif
#include "driver/temperature_sensor.h"
#if SOC_SDMMC_HOST_SUPPORTED
#include "driver/sdmmc_host.h"
#endif
#include "driver/sdspi_host.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_continuous.h"
#if SOC_LCD_RGB_SUPPORTED
#include "esp_lcd_panel_rgb.h"
#endif
#if SOC_USB_OTG_SUPPORTED
#include "usb/usb_host.h"
#endif
#include "esp_wifi.h"
#include "nimble/nimble_port.h"
#include "esp_netif.h"
#include "esp_vfs_fat.h"
#include "esp_tls.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#if SOC_USB_OTG_SUPPORTED
#include "tinyusb.h"
#endif
#if SOC_USB_OTG_SUPPORTED
#include "tinyusb_cdc_acm.h"
#endif
#include "driver/usb_serial_jtag.h"
/* volatile prevents LTO and section GC from turning this into a fake audit. */
static uintptr_t volatile entry_points[] = {
    (uintptr_t)gpio_config,
    (uintptr_t)spi_bus_initialize,
    (uintptr_t)spi_bus_add_device,
    (uintptr_t)spi_device_transmit,
    (uintptr_t)i2c_new_master_bus,
    (uintptr_t)i2c_master_transmit_receive,
    (uintptr_t)uart_driver_install,
    (uintptr_t)uart_read_bytes,
    (uintptr_t)i2s_new_channel,
    (uintptr_t)ledc_timer_config,
    (uintptr_t)rmt_new_tx_channel,
    (uintptr_t)gptimer_new_timer,
#if SOC_MCPWM_SUPPORTED
    (uintptr_t)mcpwm_new_timer,
#endif
#if SOC_PCNT_SUPPORTED
    (uintptr_t)pcnt_new_unit,
#endif
    (uintptr_t)temperature_sensor_install,
    (uintptr_t)adc_oneshot_new_unit,
    (uintptr_t)adc_continuous_new_handle,
#if SOC_SDMMC_HOST_SUPPORTED
    (uintptr_t)sdmmc_host_init,
#endif
    (uintptr_t)sdspi_host_init,
#if SOC_LCD_RGB_SUPPORTED
    (uintptr_t)esp_lcd_new_rgb_panel,
#endif
#if SOC_USB_OTG_SUPPORTED
    (uintptr_t)usb_host_install,
#endif
    (uintptr_t)esp_wifi_init,
    (uintptr_t)nimble_port_init,
    (uintptr_t)esp_netif_init,
    (uintptr_t)esp_vfs_fat_register,
    (uintptr_t)esp_tls_init,
    (uintptr_t)esp_ota_begin,
    (uintptr_t)nvs_flash_init,
    (uintptr_t)usb_serial_jtag_driver_install,
    (uintptr_t)usb_serial_jtag_read_bytes,
    (uintptr_t)usb_serial_jtag_write_bytes,
#if SOC_USB_OTG_SUPPORTED
    (uintptr_t)tinyusb_driver_install,
#endif
#if SOC_USB_OTG_SUPPORTED
    (uintptr_t)tinyusb_driver_uninstall,
#endif
#if SOC_USB_OTG_SUPPORTED
    (uintptr_t)tud_cdc_n_write,
#endif
#if SOC_USB_OTG_SUPPORTED
    (uintptr_t)tud_vendor_n_write,
#endif
};
unsigned nos_esp_idf_ecosystem_link_count(void)
{
    unsigned count = sizeof(entry_points) / sizeof(entry_points[0]);
    for (unsigned i = 0; i < count; i++)
        if (!entry_points[i])
            return 0;
    return count;
}
