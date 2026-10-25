/* Copyright (C) 2026 NOS contributors. Regression-only actual erase counters. */
#include <stdlib.h>
#include "esp_partition.h"
#include "esp_err.h"
static unsigned *counts, capacity;
extern int __real_esp_partition_erase_range(const esp_partition_t *, size_t, size_t);
int __wrap_esp_partition_erase_range(const esp_partition_t *p, size_t offset, size_t size) {
    int rc = __real_esp_partition_erase_range(p, offset, size);
    if (!rc && counts && p->type == ESP_PARTITION_TYPE_DATA && p->subtype == 0x41)
        for (size_t i = offset / 4096; i < (offset + size) / 4096 && i < capacity; i++)
            counts[i]++;
    return rc;
}
int nos_esp_wl_test_begin(void) {
    const esp_partition_t *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x41, "storage");
    if (!p || counts || p->size != 0xe0000)
        return -1;
    capacity = p->size / 4096;
    counts = calloc(capacity, sizeof(*counts));
    return counts ? 0 : -1;
}
void nos_esp_wl_test_end(unsigned *distinct, unsigned *total) {
    *distinct = *total = 0;
    /* With the pinned 896KiB layout, the last three blocks are metadata.
     * All other blocks, including the moving spare, must see erases. */
    for (unsigned i = 0; counts && i + 3 < capacity; i++) {
        *distinct += counts[i] != 0;
        *total += counts[i];
    }
    free(counts);
    counts = NULL;
}
