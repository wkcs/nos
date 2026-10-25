/* Copyright (C) 2026 NOS contributors. Regression-only OSAL observation. */
#ifndef NOS_ESP_TIMING_TEST_H
#define NOS_ESP_TIMING_TEST_H
#include <stdint.h>
struct nos_timing_task {
    char name[16];
    uint32_t id, state, waiting, deleting;
    uint64_t deadline;
};
struct nos_timing_snapshot {
    uint64_t ticks, deferred;
    uint32_t suspended, count;
    struct nos_timing_task tasks[24];
};
void nos_esp_timing_snapshot(struct nos_timing_snapshot *);
void nos_esp_timing_regression(void);
void nos_esp_timing_dump(const char *stage);
int nos_esp_timing_reaped(uintptr_t handle);
void nos_esp_timing_wait_reaped(uintptr_t handle, uint32_t start);
#endif
