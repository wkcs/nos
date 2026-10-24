/* Copyright (C) 2026 NOS contributors. Minimal IDF application on NOS. */
#include <stdio.h>
#include "esp_chip_info.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nos/abi.h"
void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    printf("NOS ESP32-S3: chip=%d, physical cores=%d, scheduler cores=1, task=%ld\n", chip.model,
           chip.cores, (long)nos_syscall(NOS_SYS_GETPID, 0, 0, 0));
#ifdef NOS_IDF_ECOSYSTEM
    extern unsigned nos_esp_idf_ecosystem_link_count(void);
    printf("SDK linkage audit entries: %u\n", nos_esp_idf_ecosystem_link_count());
#endif
    puts("NOS IDF application ready");
    /* Add application services here. SDK drivers use the NOS task bridge. */
}
