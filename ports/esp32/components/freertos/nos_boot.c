/* Copyright (C) 2026 NOS contributors. */
#include "nos_port.h"
#include "esp_heap_caps_init.h"
#include "esp_rom_sys.h"
#include "esp_task_wdt.h"
#if CONFIG_SPIRAM
#include "esp_psram.h"
#include "esp_private/esp_psram_extram.h"
#endif
static unsigned initialized;
extern void nos_esp_init(void);
extern int nos_esp_create_init(TaskFunction_t entry, unsigned bytes, unsigned priority);
#ifdef NOS_ESP_VFS
extern void nos_esp_vfs_entry(void *argument);
extern int nos_esp_create_vfs(TaskFunction_t entry);
#endif

static void init_task(void *argument) {
    (void)argument;
    /* This runs on an NOS-owned task stack, so the reset stack can be freed. */
    heap_caps_enable_nonos_stack_heaps();
#if CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL
    if (esp_psram_is_initialized() &&
        esp_psram_extram_reserve_dma_pool(CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL) != ESP_OK)
        kernel_fail("ESP DMA reserve");
#endif
#if CONFIG_ESP_TASK_WDT_INIT
    esp_task_wdt_config_t watchdog = {
        .timeout_ms = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000,
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
        .idle_core_mask = 1,
#endif
#ifdef CONFIG_ESP_TASK_WDT_PANIC
        .trigger_panic = true,
#endif
    };
    if (esp_task_wdt_init(&watchdog) != ESP_OK)
        kernel_fail("ESP watchdog init");
#endif
    esp_rom_printf("NOS: init task running under NOS scheduler\n");
    nos_esp_init();
#ifdef NOS_IDF_REGRESSION
    esp_rom_printf("NOS: regression init completed\n");
    vTaskDelete(NULL);
#else
    kernel_fail("NOS init returned");
#endif
}
void nos_esp_idf_boot(void) {
    initialized = 1;
#ifdef NOS_ESP_VFS
    if (nos_esp_create_vfs(nos_esp_vfs_entry))
        kernel_fail("NOS VFS task creation");
#endif
    if (nos_esp_create_init(init_task, 8192, 1))
        kernel_fail("NOS init task creation");
}
int nos_idf_kernel_initialized(void) {
    return initialized;
}
