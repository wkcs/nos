/* Copyright (C) 2026 NOS contributors. Shared S3/C3 assertions, target labels. */
#ifndef NOS_ESP_IDF_TEST_H
#define NOS_ESP_IDF_TEST_H
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#if CONFIG_IDF_TARGET_ESP32C3
#define NOS_IDF_TEST_NAME "esp32c3"
#define NOS_IDF_TEST_CHIP CHIP_ESP32C3
#define NOS_IDF_TEST_CORES 1
#define NOS_IDF_CONSOLE_PIN 21
#define NOS_IDF_ECOSYSTEM_ENTRIES 26
#define NOS_IDF_FP_TEST "fp_math"
#else
#define NOS_IDF_TEST_NAME "esp32s3"
#define NOS_IDF_TEST_CHIP CHIP_ESP32S3
#define NOS_IDF_TEST_CORES 2
#define NOS_IDF_CONSOLE_PIN 43
#define NOS_IDF_ECOSYSTEM_ENTRIES 35
#define NOS_IDF_FP_TEST "fpu"
#endif
#endif
