/* Copyright (C) 2026 NOS contributors.
 * NOS startup policy. Hardware init tables and constructor ABI follow
 * ESP-IDF 5.4.2 startup.c (Espressif Systems, Apache-2.0).
 * The vendor OS startup and app_main dispatcher are not compiled. */
#include <stdint.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "esp_rom_sys.h"
#include "esp_private/startup_internal.h"
#include "esp_private/crosscore_int.h"
#include "esp_private/esp_int_wdt.h"
#include "nos_port.h"

#if !CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE
#error "NOS ESP startup currently supports CPU0 only"
#endif

uint64_t g_startup_time;
void start_cpu0(void) __attribute__((noreturn));
const sys_startup_fn_t g_startup_fn[1] = {start_cpu0};

static void hardware_stage(unsigned stage) {
    extern esp_system_init_fn_t _esp_system_init_fn_array_start;
    extern esp_system_init_fn_t _esp_system_init_fn_array_end;
    for (esp_system_init_fn_t *p = &_esp_system_init_fn_array_start;
         p < &_esp_system_init_fn_array_end; p++) {
        if (p->stage == stage && (p->cores & 1) && p->fn() != ESP_OK) {
            esp_rom_printf("NOS PANIC: ESP hardware init stage=%u\n", stage);
            abort();
        }
    }
}

static void constructors(void) {
#ifdef CONFIG_COMPILER_CXX_EXCEPTIONS
    struct frame_object {
        long data[10];
    };
    extern void __register_frame_info(const void *, struct frame_object *);
    extern char __eh_frame[];
    static struct frame_object object;
    __register_frame_info(__eh_frame, &object);
#endif
#ifdef NOS_IDF_RISCV
    extern void (*__init_priority_array_start[])(void);
    extern void (*__init_priority_array_end[])(void);
    for (void (**p)(void) = __init_priority_array_start; p < __init_priority_array_end; p++)
        (*p)();
#endif
    extern void (*__init_array_start[])(void);
    extern void (*__init_array_end[])(void);
    for (void (**p)(void) = __init_array_end; p != __init_array_start;)
        (*--p)();
}

void start_cpu0(void) {
    esp_rom_printf("\nNOS: bootloader handoff, target=" CONFIG_IDF_TARGET "\n");
    hardware_stage(ESP_SYSTEM_INIT_STAGE_CORE);
    constructors();
    hardware_stage(ESP_SYSTEM_INIT_STAGE_SECONDARY);
#if CONFIG_ESP_INT_WDT
    esp_int_wdt_init();
    esp_int_wdt_cpu_init();
#endif
    esp_crosscore_int_init();
    esp_rom_printf("NOS: hardware ready; starting NOS kernel\n");
    kernel_start();
}
