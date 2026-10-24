# Copyright (C) 2026 NOS contributors.
# Preserve SDK API/collection compatibility, replace tasks.c with NOS scheduling.
idf_build_get_property(non_os NON_OS_BUILD)
if(non_os)
    idf_component_register()
    return()
endif()
if(NOT IDF_TARGET STREQUAL "esp32s3" AND NOT IDF_TARGET STREQUAL "esp32c3")
    message(FATAL_ERROR "The NOS ESP-IDF port targets ESP32-S3 and ESP32-C3")
endif()
if(IDF_TARGET STREQUAL "esp32c3")
    set(sdk_arch riscv)
    set(nos_arch NOS_IDF_RISCV=1)
    set(nos_cpu riscv32)
    set(nos_pages 20)
else()
    set(sdk_arch xtensa)
    set(nos_arch NOS_XTENSA=1)
    set(nos_cpu xtensa)
    set(nos_pages 28)
endif()
set(NOS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../..")
set(SDK_RTOS "$ENV{IDF_PATH}/components/freertos")
set(KERNEL "${SDK_RTOS}/FreeRTOS-Kernel")
execute_process(COMMAND "${PYTHON}" "${NOS_ROOT}/scripts/embed_initramfs.py"
    --output "${CMAKE_CURRENT_BINARY_DIR}/empty_initramfs.c" COMMAND_ERROR_IS_FATAL ANY)
set(SRCS "${NOS_ROOT}/arch/${nos_cpu}/esp_context.c" "${NOS_ROOT}/arch/${nos_cpu}/esp_entry.S"
    "${CMAKE_CURRENT_BINARY_DIR}/empty_initramfs.c" "nos_tasks.c" "${NOS_ROOT}/ports/esp32/platform.c"
    "${SDK_RTOS}/heap_idf.c" "${NOS_ROOT}/ports/esp32/startup.c"
    "${SDK_RTOS}/port_common.c" "${SDK_RTOS}/port_systick.c"
    "${KERNEL}/list.c" "${KERNEL}/queue.c" "${KERNEL}/timers.c"
    "${KERNEL}/event_groups.c" "${KERNEL}/stream_buffer.c"
    "${KERNEL}/portable/${sdk_arch}/port.c" "${KERNEL}/portable/${sdk_arch}/portasm.S"
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
if(sdk_arch STREQUAL "xtensa")
    list(APPEND SRCS "${KERNEL}/portable/xtensa/xtensa_init.c"
        "${KERNEL}/portable/xtensa/xtensa_overlay_os_hook.c")
endif()
idf_component_register(SRCS ${SRCS}
    INCLUDE_DIRS "${SDK_RTOS}/config/include" "${SDK_RTOS}/config/include/freertos"
      "${SDK_RTOS}/config/${sdk_arch}/include" "${KERNEL}/include"
      "${KERNEL}/portable/${sdk_arch}/include" "${KERNEL}/portable/${sdk_arch}/include/freertos"
      "${SDK_RTOS}/esp_additions/include" "${NOS_ROOT}/include" "."
    PRIV_INCLUDE_DIRS "${SDK_RTOS}" "${KERNEL}/include/freertos"
    PRIV_REQUIRES esp_timer esp_system esp_hw_support esp_rom newlib
    LDFRAGMENTS "${SDK_RTOS}/linker_common.lf" "${SDK_RTOS}/linker.lf")
target_compile_definitions(${COMPONENT_LIB} PUBLIC ${nos_arch} NOS_ESP_IDF=1 NOS_SMP=1 NOS_NORMAL_BOOT=1
    PAGE_POOL_PAGES=${nos_pages} USER_BYTES=4096 HEAP_END=0)
# Native task records plus two shared-heap pages and two spare pages.
# C3: 16 tasks / 20 pages; S3: 24 tasks / 28 pages. SDK stacks use its heap.
set_source_files_properties("${KERNEL}/portable/${sdk_arch}/port.c" PROPERTIES
    COMPILE_DEFINITIONS "offset_pxEndOfStack=nos_sdk_unused_stack_offset")
set_source_files_properties("${KERNEL}/queue.c" "${KERNEL}/timers.c"
    "${KERNEL}/event_groups.c" "${KERNEL}/stream_buffer.c"
    PROPERTIES COMPILE_DEFINITIONS "_ESP_FREERTOS_INTERNAL")
idf_component_set_property(freertos ORIG_INCLUDE_PATH "${KERNEL}/include/freertos/")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u nos_esp_init" "-T${CMAKE_CURRENT_LIST_DIR}/shared.ld")

# NOS internal quoted headers must not override SDK/Newlib system headers.
target_compile_options(${COMPONENT_LIB} PRIVATE "-iquote${NOS_ROOT}/include/nos")

# Keep the SDK's collection allocation helpers. Task-with-caps lifecycle is NOS
# owned; the renamed SDK alternatives must be discarded by section GC.
set_source_files_properties("${SDK_RTOS}/esp_additions/idf_additions.c" PROPERTIES
    COMPILE_DEFINITIONS "xTaskCreatePinnedToCoreWithCaps=nos_sdk_unused_TaskCreateCaps;vTaskDeleteWithCaps=nos_sdk_unused_DeleteCaps")

# The task/IRQ bridge is deliberately a single-core, 100 Hz profile. Reject
# incompatible configurations instead of silently booting with the wrong ABI.
if(NOT CONFIG_FREERTOS_UNICORE OR CONFIG_FREERTOS_SMP OR NOT CONFIG_FREERTOS_HZ EQUAL 100)
    message(FATAL_ERROR "NOS ESP-IDF currently requires single core, IDF RTOS ABI, 100 Hz")
endif()
if(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS OR CONFIG_FREERTOS_USE_TICKLESS_IDLE
   OR CONFIG_FREERTOS_TASK_PRE_DELETION_HOOK OR CONFIG_ESP_SYSTEM_HW_STACK_GUARD)
    message(FATAL_ERROR "Runtime stats, tickless idle, SDK TCB hooks and hardware stack guard are not ported")
endif()
if(NOT CONFIG_ESP_CONSOLE_UART_DEFAULT OR NOT CONFIG_ESP_CONSOLE_SECONDARY_NONE)
    message(FATAL_ERROR "NOS audited IDF profiles require the default UART0 console with no secondary console")
endif()
if(NOT "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}" VERSION_EQUAL "5.4.2")
    message(FATAL_ERROR "NOS ESP-IDF requires the audited ESP-IDF 5.4.2 ABI")
endif()

# NOS owns the image entry and system startup. The vendor CPU routine is
# retained only for reset/cache/clock/ROM/heap hardware bring-up.
idf_build_get_property(nos_regression NOS_IDF_REGRESSION)
if(nos_regression)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NOS_IDF_REGRESSION=1)
endif()
set_source_files_properties("${NOS_ROOT}/ports/esp32/startup.c"
    PROPERTIES COMPILE_OPTIONS "-fno-stack-protector")
