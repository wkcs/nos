# Copyright (C) 2026 NOS contributors.
# Include after project(): NOS owns the image entry and OS startup policy.
idf_component_get_property(esp_system_lib esp_system COMPONENT_LIB)
get_target_property(esp_system_sources ${esp_system_lib} SOURCES)
list(FILTER esp_system_sources EXCLUDE REGEX "(^|/)startup\\.c$")
set_property(TARGET ${esp_system_lib} PROPERTY SOURCES "${esp_system_sources}")
set_property(SOURCE "$ENV{IDF_PATH}/components/esp_system/port/cpu_start.c"
    TARGET_DIRECTORY ${esp_system_lib} APPEND PROPERTY COMPILE_DEFINITIONS
    call_start_cpu0=nos_esp_hal_cpu_init)
