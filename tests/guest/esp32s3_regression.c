/* Copyright (C) 2026 NOS contributors. ESP32-S3 on-target regression. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/stream_buffer.h"
#include "freertos/timers.h"
#include "esp_chip_info.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_ota_ops.h"
#include "nos/abi.h"
static QueueHandle_t queue;
static SemaphoreHandle_t done;
static EventGroupHandle_t events;
static StreamBufferHandle_t stream;
static TaskHandle_t main_handle;
static SemaphoreHandle_t inheritance_mutex;
static volatile unsigned isr_seen;
static volatile unsigned spins[2], stop_spinning, tls_deleted;
static __thread uint32_t thread_cookie;
static void spinner(void *arg)
{
    unsigned n = (uintptr_t)arg;
    while (!stop_spinning)
        spins[n]++;
    xSemaphoreGive(done);
    vTaskDelete(NULL);
}
static void queue_consumer(void *unused)
{
    (void)unused;
    uint32_t value = 0;
    assert(xQueueReceive(queue, &value, pdMS_TO_TICKS(500)) == pdTRUE);
    assert(value == 0x12345678);
    assert(xQueueReceive(queue, &value, 2) == pdFALSE);
    xSemaphoreGive(done);
    vTaskDelete(NULL);
}
static void tls_cleanup(int index, void *pointer)
{
    assert(index == 0 && pointer == (void *)0x1234);
    tls_deleted++;
}
static void tls_worker(void *arg)
{
    unsigned tag = (uintptr_t)arg;
    thread_cookie = tag;
    errno = 20 + tag;
    volatile float number = (float)tag;
    vTaskSetThreadLocalStoragePointerAndDelCallback(NULL, 0, (void *)0x1234, tls_cleanup);
    for (unsigned i = 0; i < 16; i++) {
        number += 0.5f;
        vTaskDelay(1);
        assert(thread_cookie == tag && errno == (int)(20 + tag));
        assert(number == (float)tag + (i + 1) * 0.5f);
        assert(pvTaskGetThreadLocalStoragePointer(NULL, 0) == (void *)0x1234);
    }
    xSemaphoreGive(done);
    vTaskDelete(NULL);
}
static void event_worker(void *unused)
{
    (void)unused;
    assert((xEventGroupWaitBits(events, 3, pdTRUE, pdTRUE, 100) & 3) == 3);
    char data[8] = {0};
    assert(xStreamBufferReceive(stream, data, 5, 100) == 5);
    assert(!memcmp(data, "hello", 5));
    xSemaphoreGive(done);
    vTaskDelete(NULL);
}
static void timer_callback(TimerHandle_t timer)
{
    (void)timer;
    xSemaphoreGive(done);
}
static void esp_callback(void *unused)
{
    (void)unused;
    xTaskNotifyGive(main_handle);
}
static void isr_callback(void *unused)
{
    (void)unused;
    assert(xPortInIsrContext());
    assert(nos_syscall(NOS_SYS_GETPID, 0, 0, 0) == -NOS_EDEADLK);
    isr_seen++;
    BaseType_t higher = pdFALSE;
    vTaskNotifyGiveFromISR(main_handle, &higher);
    if (higher)
        esp_timer_isr_dispatch_need_yield();
}
static void mutex_waiter(void *unused)
{
    (void)unused;
    assert(xSemaphoreTake(inheritance_mutex, 100) == pdTRUE);
    assert(xSemaphoreGive(inheritance_mutex) == pdTRUE);
    xSemaphoreGive(done);
    vTaskDelete(NULL);
}
static void passive(void *unused)
{
    (void)unused;
    vTaskSuspend(NULL);
    assert(0);
}
static void wait_done(unsigned count)
{
    for (unsigned i = 0; i < count; i++)
        assert(xSemaphoreTake(done, pdMS_TO_TICKS(3000)) == pdTRUE);
    vTaskDelay(2); /* Give the NOS idle task a chance to reclaim terminated tasks. */
}
static void short_lived(void *unused)
{
    (void)unused;
    xSemaphoreGive(done);
    vTaskDelete(NULL);
}
static uintptr_t endpoint, client_pid;
static void ipc_client(void *unused)
{
    (void)unused;
    client_pid = nos_syscall(NOS_SYS_GETPID, 0, 0, 0);
    xSemaphoreGive(done);
    ulTaskNotifyTake(pdTRUE, 100);
    struct nos_message request = {.value = 41}, reply;
    assert(nos_syscall(NOS_SYS_CALL, endpoint, (uintptr_t)&request, (uintptr_t)&reply) == 0);
    assert(reply.value == 42);
    xSemaphoreGive(done);
    vTaskDelete(NULL);
}
void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    assert(chip.model == CHIP_ESP32S3);
    printf("NOS ESP32-S3 app_main chip=%d cores=%d\n", chip.model, chip.cores);
#ifdef NOS_IDF_ECOSYSTEM
    extern unsigned nos_esp_idf_ecosystem_link_count(void);
    assert(nos_esp_idf_ecosystem_link_count() == 32);
    puts("PASS esp32s3_sdk_ecosystem_linkage");
#endif
    assert(xTaskGetSchedulerState() == taskSCHEDULER_RUNNING);
    main_handle = xTaskGetCurrentTaskHandle();
    done = xSemaphoreCreateCounting(8, 0);
    assert(done);
    assert(nos_syscall(NOS_SYS_GETPID, 0, 0, 0) >= 0);
    assert(nos_syscall(NOS_SYS_USER_CHECK, 0, 4, NOS_PROT_READ) == -NOS_EFAULT);
    char sample[8];
    assert(nos_syscall(NOS_SYS_USER_COPY, (uintptr_t)sample, (uintptr_t) "native", 7) == 0);
    assert(!strcmp(sample, "native"));
    vTaskSuspendAll();
    vTaskSuspendAll();
    int64_t until = esp_timer_get_time() + 30000;
    while (esp_timer_get_time() < until) {
    }
    assert(nos_syscall(NOS_SYS_GETPID, 0, 0, 0) == -NOS_EDEADLK);
    assert(xTaskResumeAll() == pdFALSE);
    assert(xTaskGetSchedulerState() == taskSCHEDULER_SUSPENDED);
    xTaskResumeAll();
    assert(xTaskGetSchedulerState() == taskSCHEDULER_RUNNING);
    struct nos_message no_vfs = {0};
    assert(nos_syscall(NOS_SYS_CALL, 0, (uintptr_t)&no_vfs, (uintptr_t)&no_vfs) == -NOS_EACCES);
    assert(nos_syscall(NOS_SYS_SERVICE_RESTART, 0, 0, 0) == -NOS_ENOTSUP);
    puts("PASS esp32s3_native_syscalls");

    assert(xTaskCreate(spinner, "spin0", 2048, (void *)0, 1, NULL) == pdPASS);
    assert(xTaskCreate(spinner, "spin1", 2048, (void *)1, 1, NULL) == pdPASS);
    vTaskDelay(10);
    assert(spins[0] && spins[1]);
    stop_spinning = 1;
    wait_done(2);
    puts("PASS esp32s3_timer_preemption");

    queue = xQueueCreate(2, sizeof(uint32_t));
    assert(queue);
    assert(xTaskCreate(queue_consumer, "queue", 3072, NULL, 3, NULL) == pdPASS);
    uint32_t value = 0x12345678;
    assert(xQueueSend(queue, &value, 10) == pdTRUE);
    wait_done(1);
    vQueueDelete(queue);
    puts("PASS esp32s3_queue_timeout");

    errno = 7;
    assert(xTaskCreate(tls_worker, "tls1", 4096, (void *)1, 3, NULL) == pdPASS);
    assert(xTaskCreate(tls_worker, "tls2", 4096, (void *)2, 3, NULL) == pdPASS);
    wait_done(2);
    assert(errno == 7 && thread_cookie == 0 && tls_deleted == 2);
    puts("PASS esp32s3_tls_errno_fpu_cleanup");

    events = xEventGroupCreate();
    stream = xStreamBufferCreate(32, 1);
    assert(events && stream);
    assert(xTaskCreate(event_worker, "events", 4096, NULL, 3, NULL) == pdPASS);
    xEventGroupSetBits(events, 1);
    vTaskDelay(1);
    xEventGroupSetBits(events, 2);
    assert(xStreamBufferSend(stream, "hello", 5, 10) == 5);
    wait_done(1);
    assert((xEventGroupGetBits(events) & 3) == 0);
    vEventGroupDelete(events);
    vStreamBufferDelete(stream);
    puts("PASS esp32s3_event_stream");

    TimerHandle_t timer = xTimerCreate("test", 2, pdFALSE, NULL, timer_callback);
    assert(timer && xTimerStart(timer, 10) == pdPASS);
    wait_done(1);
    assert(xTimerDelete(timer, 10) == pdPASS);
    esp_timer_handle_t esp_timer;
    esp_timer_create_args_t args = {.callback = esp_callback, .name = "nos-test"};
    assert(esp_timer_create(&args, &esp_timer) == ESP_OK);
    assert(esp_timer_start_once(esp_timer, 20000) == ESP_OK);
    assert(ulTaskNotifyTake(pdTRUE, 100) == 1);
    assert(esp_timer_delete(esp_timer) == ESP_OK);
    args.callback = isr_callback;
    args.dispatch_method = ESP_TIMER_ISR;
    assert(esp_timer_create(&args, &esp_timer) == ESP_OK);
    assert(esp_timer_start_once(esp_timer, 20000) == ESP_OK);
    assert(ulTaskNotifyTake(pdTRUE, 100) == 1 && isr_seen == 1);
    assert(esp_timer_delete(esp_timer) == ESP_OK);
    assert(ulTaskNotifyTake(pdTRUE, 2) == 0);
    assert(xTaskNotify(main_handle, 0x55, eSetValueWithoutOverwrite) == pdPASS);
    assert(xTaskNotify(main_handle, 0xaa, eSetValueWithoutOverwrite) == pdFAIL);
    uint32_t notified = 0;
    assert(xTaskNotifyWait(0, UINT32_MAX, &notified, 0) == pdTRUE && notified == 0x55);
    puts("PASS esp32s3_timers_notifications");

    endpoint = nos_syscall(NOS_SYS_ENDPOINT_CREATE, 0, 0, 0);
    assert((intptr_t)endpoint >= 0);
    TaskHandle_t client;
    assert(xTaskCreate(ipc_client, "ipc", 4096, NULL, 3, &client) == pdPASS);
    wait_done(1);
    assert(nos_syscall(NOS_SYS_ENDPOINT_GRANT, client_pid, endpoint, NOS_RIGHT_CALL) == 0);
    xTaskNotifyGive(client);
    struct nos_message message;
    assert(nos_syscall(NOS_SYS_RECV, endpoint, (uintptr_t)&message, 0) == 0);
    assert(message.value == 41);
    message.value++;
    assert(nos_syscall(NOS_SYS_REPLY, client_pid, (uintptr_t)&message, 0) == 0);
    wait_done(1);
    assert(nos_syscall(NOS_SYS_ENDPOINT_CLOSE, endpoint, 0, 0) == 0);
    puts("PASS esp32s3_native_ipc");

    SemaphoreHandle_t mutex = xSemaphoreCreateRecursiveMutex();
    assert(mutex && xSemaphoreTakeRecursive(mutex, 1) == pdTRUE);
    assert(xSemaphoreTakeRecursive(mutex, 1) == pdTRUE);
    assert(xSemaphoreGiveRecursive(mutex) == pdTRUE);
    assert(xSemaphoreGiveRecursive(mutex) == pdTRUE);
    vSemaphoreDelete(mutex);
    inheritance_mutex = xSemaphoreCreateMutex();
    assert(inheritance_mutex && xSemaphoreTake(inheritance_mutex, 1) == pdTRUE);
    assert(xTaskCreate(mutex_waiter, "pi", 3072, NULL, 3, NULL) == pdPASS);
    assert(uxTaskPriorityGet(NULL) == 3);
    assert(xTaskCreate(short_lived, "pi-exit", 3072, NULL, 4, NULL) == pdPASS);
    assert(uxTaskPriorityGet(NULL) == 3); /* Native exit must preserve SDK donation. */
    assert(xSemaphoreGive(inheritance_mutex) == pdTRUE);
    wait_done(2);
    assert(uxTaskPriorityGet(NULL) == 1);
    vSemaphoreDelete(inheritance_mutex);
    puts("PASS esp32s3_priority_inheritance_native_exit");

    static StackType_t static_stack[4096] __attribute__((aligned(16)));
    static StaticTask_t static_tcb;
    TaskHandle_t static_task = xTaskCreateStatic(passive, "static", sizeof(static_stack), NULL, 0,
                                                 static_stack, &static_tcb);
    assert(static_task);
    StackType_t *stack;
    StaticTask_t *tcb;
    assert(xTaskGetStaticBuffers(static_task, &stack, &tcb) == pdTRUE);
    assert(stack == static_stack && tcb == &static_tcb);
    vTaskDelete(static_task);
    assert(eTaskGetState(static_task) == eDeleted);
    TaskHandle_t capacity[32];
    unsigned created = 0;
    size_t capacity_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    while (created < 32 &&
           xTaskCreate(passive, "limit", 2048, NULL, 0, &capacity[created]) == pdPASS)
        created++;
    assert(created > 0 && created < 32 && capacity[created] == NULL);
    for (unsigned i = 0; i < created; i++)
        vTaskDelete(capacity[i]);
    assert(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) == capacity_free);
    puts("PASS esp32s3_static_tasks_capacity_rollback");

    gpio_config_t pins = {.pin_bit_mask = 1ULL << 4,
                          .mode = GPIO_MODE_INPUT,
                          .pull_up_en = GPIO_PULLUP_DISABLE,
                          .pull_down_en = GPIO_PULLDOWN_ENABLE};
    assert(gpio_config(&pins) == ESP_OK);
    assert(gpio_get_level(GPIO_NUM_4) == 0 || gpio_get_level(GPIO_NUM_4) == 1);
    uart_config_t uart = {.baud_rate = 115200,
                          .data_bits = UART_DATA_8_BITS,
                          .parity = UART_PARITY_DISABLE,
                          .stop_bits = UART_STOP_BITS_1,
                          .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                          .source_clk = UART_SCLK_DEFAULT};
    assert(uart_param_config(UART_NUM_1, &uart) == ESP_OK);
    assert(uart_driver_install(UART_NUM_1, 512, 0, 0, NULL, 0) == ESP_OK);
    uint8_t byte;
    assert(uart_read_bytes(UART_NUM_1, &byte, 1, 0) == 0);
    assert(uart_write_bytes(UART_NUM_1, "NOS", 3) == 3);
    assert(uart_driver_delete(UART_NUM_1) == ESP_OK);
    assert(nvs_flash_init() == ESP_OK);
    nvs_handle_t nvs;
    assert(nvs_open("nos-test", NVS_READWRITE, &nvs) == ESP_OK);
    assert(nvs_set_u32(nvs, "value", 0x12345678) == ESP_OK && nvs_commit(nvs) == ESP_OK);
    uint32_t stored;
    assert(nvs_get_u32(nvs, "value", &stored) == ESP_OK && stored == 0x12345678);
    assert(nvs_erase_all(nvs) == ESP_OK && nvs_commit(nvs) == ESP_OK);
    nvs_close(nvs);
    puts("PASS esp32s3_sdk_gpio_uart_flash_nvs");
    const esp_partition_t *active = esp_ota_get_running_partition();
    const esp_partition_t *inactive = esp_ota_get_next_update_partition(NULL);
    assert(active && inactive && active->address != inactive->address);
    esp_ota_handle_t ota;
    assert(esp_ota_begin(inactive, 4096, &ota) == ESP_OK);
    static const uint8_t invalid_image[32] = {0};
    assert(esp_ota_write(ota, invalid_image, sizeof(invalid_image)) == ESP_ERR_OTA_VALIDATE_FAILED);
    assert(esp_ota_abort(ota) == ESP_OK);
    assert(esp_ota_get_running_partition()->address == active->address);
    puts("PASS esp32s3_sdk_ota_reject_abort");

    size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    for (unsigned i = 0; i < 40; i++) {
        assert(xTaskCreate(short_lived, "recycle", 3072, NULL, 3, NULL) == pdPASS);
        wait_done(1);
    }
    assert(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) == before);
    puts("PASS esp32s3_task_recycle_no_stack_leak");
    puts("PASS esp32s3_nos_idf_regression");
    fflush(stdout);
}
