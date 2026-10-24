/* Copyright (C) 2026 NOS contributors. Minimal IDF application on NOS. */
#include <stdio.h>
#include "esp_chip_info.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nos/abi.h"
#include "nos/driver/esp_idf.h"
#include "nos/esp_shell.h"
#include <assert.h>
void app_main(void)
{
    assert(nos_idf_devices_init() == 0);
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    printf("NOS " CONFIG_IDF_TARGET ": chip=%d, physical cores=%d, scheduler cores=1, task=%ld\n", chip.model,
           chip.cores, (long)nos_syscall(NOS_SYS_GETPID, 0, 0, 0));
#ifdef NOS_IDF_ECOSYSTEM
    extern unsigned nos_esp_idf_ecosystem_link_count(void);
    printf("SDK linkage audit entries: %u\n", nos_esp_idf_ecosystem_link_count());
#endif
    puts("NOS IDF application ready");
    /* Register board devices through nos_idf_device_create(); applications
     * use NOS service requests, never vendor controller handles. */
    nos_idf_shell_run();
}
