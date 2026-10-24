# Copyright (C) 2026 NOS contributors.
idf_build_get_property(NOS_IDF_ECOSYSTEM NOS_IDF_ECOSYSTEM)
idf_build_get_property(NOS_IDF_REGRESSION NOS_IDF_REGRESSION)
if(NOS_IDF_REGRESSION)
    set(sources "${CMAKE_CURRENT_LIST_DIR}/../../../../tests/guest/esp32s3_regression.c")
else()
    set(sources "main.c")
endif()
set(dependencies freertos esp_timer esp_driver_gpio esp_driver_uart nvs_flash app_update)
if(NOS_IDF_ECOSYSTEM)
    list(APPEND sources "ecosystem.c")
    list(APPEND dependencies esp_driver_spi esp_driver_i2c esp_driver_i2s esp_driver_ledc
        esp_driver_rmt esp_driver_gptimer esp_driver_mcpwm esp_driver_pcnt esp_driver_tsens
        esp_driver_sdmmc esp_driver_sdspi esp_adc esp_lcd usb esp_wifi bt esp_netif fatfs
        esp-tls app_update esp_tinyusb)
endif()
idf_component_register(SRCS ${sources} REQUIRES ${dependencies})
if(NOS_IDF_ECOSYSTEM)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NOS_IDF_ECOSYSTEM=1)
endif()
