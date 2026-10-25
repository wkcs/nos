/* Copyright (C) 2026 NOS contributors. Private raw NOR partition backend. */
#ifndef NOS_ESP_FLASH_BLOCK_H
#define NOS_ESP_FLASH_BLOCK_H
#include "nos/driver/service_io.h"
#include "esp_partition.h"
struct nos_flash_block {
    const esp_partition_t *partition;
};
int nos_flash_block_start(struct nos_flash_block *);
int nos_flash_block_stop(struct nos_flash_block *);
int nos_flash_block_call(struct nos_flash_block *, uint32_t, void *, size_t);
#endif
