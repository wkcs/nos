# Copyright (C) 2026 NOS contributors.
# Preserve SDK API/collection compatibility, replace tasks.c with NOS scheduling.
idf_build_get_property(non_os NON_OS_BUILD)
if(non_os)
    idf_component_register()
    return()
endif()
if(NOT IDF_TARGET STREQUAL "esp32s3")
    message(FATAL_ERROR "The NOS ESP-IDF port currently targets ESP32-S3")
endif()
set(NOS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../..")
set(SDK_RTOS "$ENV{IDF_PATH}/components/freertos")
set(KERNEL "${SDK_RTOS}/FreeRTOS-Kernel")
execute_process(COMMAND "${PYTHON}" "${NOS_ROOT}/scripts/embed_initramfs.py"
    --output "${CMAKE_CURRENT_BINARY_DIR}/empty_initramfs.c" COMMAND_ERROR_IS_FATAL ANY)
set(SRCS "${CMAKE_CURRENT_BINARY_DIR}/empty_initramfs.c" "nos_tasks.c" "${NOS_ROOT}/arch/xtensa/platform.c"
    "${SDK_RTOS}/heap_idf.c" "${SDK_RTOS}/app_startup.c"
    "${SDK_RTOS}/port_common.c" "${SDK_RTOS}/port_systick.c"
    "${KERNEL}/list.c" "${KERNEL}/queue.c" "${KERNEL}/timers.c"
    "${KERNEL}/event_groups.c" "${KERNEL}/stream_buffer.c"
    "${KERNEL}/portable/xtensa/port.c" "${KERNEL}/portable/xtensa/portasm.S"
    "${KERNEL}/portable/xtensa/xtensa_init.c"
    "${KERNEL}/portable/xtensa/xtensa_overlay_os_hook.c"
    "${SDK_RTOS}/esp_additions/freertos_compatibility.c"
    "${SDK_RTOS}/esp_additions/idf_additions.c"
    "${SDK_RTOS}/esp_additions/idf_additions_event_groups.c"
    "${NOS_ROOT}/kernel/core/core.c" "${NOS_ROOT}/kernel/core/runtime.c"
    "${NOS_ROOT}/kernel/core/credentials.c"
    "${NOS_ROOT}/kernel/sched/task.c" "${NOS_ROOT}/kernel/sched/scheduler.c"
    "${NOS_ROOT}/kernel/ipc/ipc.c" "${NOS_ROOT}/kernel/ipc/irq.c" "${NOS_ROOT}/kernel/ipc/mutex.c"
    "${NOS_ROOT}/kernel/mm/memory.c" "${NOS_ROOT}/kernel/exec/elf.c" "${NOS_ROOT}/kernel/exec/bootfile.c"
    "${NOS_ROOT}/kernel/compat/linux/linux.c" "${NOS_ROOT}/kernel/compat/linux/linux_signal.c"
    "${NOS_ROOT}/user/lib/syscall.c" "${NOS_ROOT}/user/lib/process.c"
    "nos_boot.c")
idf_component_register(SRCS ${SRCS}
    INCLUDE_DIRS "${SDK_RTOS}/config/include" "${SDK_RTOS}/config/include/freertos"
      "${SDK_RTOS}/config/xtensa/include" "${KERNEL}/include"
      "${KERNEL}/portable/xtensa/include" "${KERNEL}/portable/xtensa/include/freertos"
      "${SDK_RTOS}/esp_additions/include" "${NOS_ROOT}/include" "."
    PRIV_INCLUDE_DIRS "${SDK_RTOS}" "${KERNEL}/include/freertos"
    PRIV_REQUIRES esp_timer esp_system esp_hw_support esp_rom newlib
    LDFRAGMENTS "${SDK_RTOS}/linker_common.lf" "${SDK_RTOS}/linker.lf")
target_compile_definitions(${COMPONENT_LIB} PUBLIC NOS_XTENSA=1 NOS_ESP_IDF=1 NOS_SMP=1 NOS_NORMAL_BOOT=1
    PAGE_POOL_PAGES=28 USER_BYTES=4096 HEAP_END=0)
# 24 native task records + 2 shared-heap pages fit in 28 pages. Leave 16 KiB
# more internal SRAM for the SDK GATT/bond stores and driver heap.
set_source_files_properties("${KERNEL}/portable/xtensa/port.c" PROPERTIES
    COMPILE_DEFINITIONS "offset_pxEndOfStack=nos_sdk_unused_stack_offset")
set_source_files_properties("${KERNEL}/queue.c" "${KERNEL}/timers.c"
    "${KERNEL}/event_groups.c" "${KERNEL}/stream_buffer.c"
    PROPERTIES COMPILE_DEFINITIONS "_ESP_FREERTOS_INTERNAL")
idf_component_set_property(freertos ORIG_INCLUDE_PATH "${KERNEL}/include/freertos/")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u app_main" "-T${CMAKE_CURRENT_LIST_DIR}/shared.ld")

# NOS internal quoted headers must not override SDK/Newlib system headers.
target_compile_options(${COMPONENT_LIB} PRIVATE "-iquote${NOS_ROOT}/include/nos")

# Keep the SDK's collection allocation helpers. Task-with-caps lifecycle is NOS
# owned; the renamed SDK alternatives must be discarded by section GC.
set_source_files_properties("${SDK_RTOS}/esp_additions/idf_additions.c" PROPERTIES
    COMPILE_DEFINITIONS "xTaskCreatePinnedToCoreWithCaps=nos_sdk_unused_TaskCreateCaps;vTaskDeleteWithCaps=nos_sdk_unused_DeleteCaps")

# The task/IRQ bridge is deliberately a single-core, 100 Hz profile. Reject
# incompatible configurations instead of silently booting with the wrong ABI.
if(NOT CONFIG_FREERTOS_UNICORE OR CONFIG_FREERTOS_SMP OR NOT CONFIG_FREERTOS_HZ EQUAL 100)
    message(FATAL_ERROR "NOS ESP32-S3 currently requires single core, IDF RTOS ABI, 100 Hz")
endif()
if(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS OR CONFIG_FREERTOS_USE_TICKLESS_IDLE
   OR CONFIG_FREERTOS_TASK_PRE_DELETION_HOOK)
    message(FATAL_ERROR "Runtime stats, tickless idle and custom SDK TCB hooks are not ported")
endif()
if(NOT "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}" VERSION_EQUAL "5.4.2")
    message(FATAL_ERROR "NOS ESP32-S3 requires the audited ESP-IDF 5.4.2 ABI")
endif()
