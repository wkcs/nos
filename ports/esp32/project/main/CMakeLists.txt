# Copyright (C) 2026 NOS contributors.
idf_build_get_property(NOS_IDF_ECOSYSTEM NOS_IDF_ECOSYSTEM)
idf_build_get_property(NOS_IDF_REGRESSION NOS_IDF_REGRESSION)
idf_build_get_property(storage_wl NOS_IDF_STORAGE_WL)
if(NOS_IDF_REGRESSION)
    set(sources "${CMAKE_CURRENT_LIST_DIR}/../../../../tests/guest/esp32s3_regression.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../../../tests/guest/esp32s3_protocols.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../../../tests/guest/esp32_timing_regression.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../../../tests/guest/esp32_vfs_regression.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../../../tests/guest/esp32_libc_regression.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../../../tests/guest/esp32_storage_regression.c")
else()
    set(sources "${CMAKE_CURRENT_LIST_DIR}/../../init.c")
endif()
execute_process(COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../../../../scripts/check_idf_app_devices.py"
    ${sources} WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}" COMMAND_ERROR_IS_FATAL ANY)
set(dependencies freertos esp_timer nos_devices nos_vfs nos_libc)
if(NOT NOS_IDF_REGRESSION)
    list(APPEND dependencies nos_shell)
endif()
idf_component_register(SRCS ${sources} REQUIRES ${dependencies})
add_custom_target(nos_device_api_policy
    COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../../../../scripts/check_idf_app_devices.py" ${sources}
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}" VERBATIM)
add_dependencies(${COMPONENT_LIB} nos_device_api_policy)
if(NOS_IDF_ECOSYSTEM)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NOS_IDF_ECOSYSTEM=1)
endif()
if(storage_wl)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NOS_IDF_STORAGE_WL=1)
endif()
