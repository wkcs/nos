/* Copyright (C) 2026 NOS contributors. Actual managed Flash and NOS snapshot. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "nos/user.h"
#include "nos/driver/esp_idf.h"
#include "esp_idf_test.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
static void mount_client(void *argument) {
    assert(nos_syscall(NOS_SYS_GETPID, 0, 0, 0) != 1);
    assert(nos_mount("/child-storage", 1) == -NOS_EPERM);
    xSemaphoreGive((SemaphoreHandle_t)argument);
}
void nos_esp_storage_regression(void) {
    assert(nos_syscall(NOS_SYS_GETPID, 0, 0, 0) == 1);
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    assert(done && xTaskCreate(mount_client, "mount-denied", 4096, done, 1, NULL) == pdPASS);
    assert(xSemaphoreTake(done, 100) == pdTRUE);
    vTaskDelay(2);
    vSemaphoreDelete(done);
    int rc = nos_mount("/data", 1);
    printf("Flash mount rc=%d\n", rc);
    assert(!rc);
    assert(nos_mount("/data", 1) == -NOS_EBUSY);
    struct nos_idf_device_config config = {.name = "test.storage-conflict",
                                           .kind = NOS_SERVICE_BLOCK};
    struct nos_service *other;
    assert(!nos_idf_device_create(&config, &other));
    assert(nos_service_start(other) == -NOS_DRV_EBUSY);
    assert(!nos_idf_device_destroy(&other));
    config.instance = 1;
    assert(nos_idf_device_create(&config, &other) == -NOS_DRV_EINVAL);
    unsigned char data[512], result[512];
    for (unsigned i = 0; i < sizeof(data); i++)
        data[i] = (i * 17u) ^ (i >> 3);
    FILE *file = fopen("/data/binary", "w+");
    assert(file && fwrite(data, 1, sizeof(data), file) == sizeof(data));
    assert(!fflush(file) && !fsync(fileno(file)));
    assert(!fseek(file, 0, SEEK_SET));
    assert(fread(result, 1, sizeof(result), file) == sizeof(result));
    assert(!memcmp(data, result, sizeof(data)) && !fclose(file));
    assert(!unlink("/data/binary") && !nos_sync());
    TaskHandle_t service = xTaskGetHandle("vfs");
    assert(service && uxTaskGetStackHighWaterMark(service) >= 512);
    printf("Flash VFS stack_free=%u\n", (unsigned)uxTaskGetStackHighWaterMark(service));
#ifdef NOS_IDF_STORAGE_WL
    extern int nos_esp_wl_test_begin(void);
    extern void nos_esp_wl_test_end(unsigned *, unsigned *);
    assert(!nos_esp_wl_test_begin());
    for (unsigned i = 0; i < 128; i++) {
        file = fopen("/data/cycle", "w");
        assert(file && fprintf(file, "cycle=%u", i) > 0 && !fclose(file));
        assert(!nos_sync());
    }
    unsigned distinct, total;
    nos_esp_wl_test_end(&distinct, &total);
    printf("WL physical_data_blocks=%u erases=%u\n", distinct, total);
    assert(distinct == 221 && total > 128 * 33);
    file = fopen("/data/cycle", "r");
    unsigned last;
    assert(file && fscanf(file, "cycle=%u", &last) == 1 && last == 127 && !fclose(file));
    puts("PASS " NOS_IDF_TEST_NAME "_wear_levelled_physical_rotation");
#endif
    puts("PASS " NOS_IDF_TEST_NAME "_native_flash_block_mount_sync");
}
