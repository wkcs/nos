# Copyright (C) 2026 NOS contributors.
set(NOS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../..")
idf_build_get_property(ecosystem NOS_IDF_ECOSYSTEM)
set(sources "devices.c" "serial_buses.c" "storage.c")
set(dependencies freertos esp_driver_gpio esp_driver_uart esp_driver_spi esp_driver_i2c
    esp_timer nvs_flash app_update)
if(ecosystem)
    list(APPEND sources "wireless.c" "usb.c" "auxiliary.c" "can.c" "ecosystem.c")
    list(APPEND dependencies esp_wifi bt esp_netif usb esp_tinyusb esp_driver_i2s esp_driver_ledc
        esp_driver_rmt esp_driver_gptimer esp_driver_mcpwm esp_driver_pcnt esp_driver_tsens
        esp_driver_sdmmc esp_driver_sdspi esp_adc esp_lcd sdmmc driver fatfs esp-tls)
endif()
idf_component_register(SRCS ${sources}
    "${NOS_ROOT}/servers/drivers/framework/device.c"
    "${NOS_ROOT}/servers/drivers/framework/peripheral.c"
    "${NOS_ROOT}/servers/drivers/framework/service.c"
    INCLUDE_DIRS "${NOS_ROOT}/include" PRIV_REQUIRES ${dependencies})
target_compile_options(${COMPONENT_LIB} PRIVATE "-iquote${NOS_ROOT}/include/nos")
if(ecosystem)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NOS_IDF_ECOSYSTEM=1)
endif()
