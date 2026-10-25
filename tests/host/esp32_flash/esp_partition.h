/* Copyright (C) 2026 NOS contributors. Fault-model SDK boundary only. */
#ifndef TEST_ESP_PARTITION_H
#define TEST_ESP_PARTITION_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef unsigned esp_partition_subtype_t;
#define ESP_PARTITION_TYPE_DATA 1
typedef struct {
    unsigned type, subtype;
    size_t address, size;
    bool encrypted, readonly;
    size_t erase_size;
} esp_partition_t;
#ifdef __cplusplus
extern "C" {
#endif
const esp_partition_t *esp_partition_find_first(unsigned, esp_partition_subtype_t, const char *);
int esp_partition_read(const esp_partition_t *, size_t, void *, size_t);
int esp_partition_write(const esp_partition_t *, size_t, const void *, size_t);
int esp_partition_erase_range(const esp_partition_t *, size_t, size_t);
#ifdef __cplusplus
}
#endif
#endif
