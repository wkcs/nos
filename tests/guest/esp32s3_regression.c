/* Copyright (C) 2026 NOS contributors. ESP32-S3 on-target regression. */
#include <assert.h>
#include "esp_idf_test.h"
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
#include "nos/abi.h"
#include "nos/user.h"
#include "nos/driver/esp_idf.h"
#include "nos/driver/usb_service.h"
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
void nos_esp_init(void)
{
    extern void nos_esp32s3_protocol_regression(void);
    nos_esp32s3_protocol_regression();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    assert(chip.model == NOS_IDF_TEST_CHIP);
    assert(chip.cores == NOS_IDF_TEST_CORES);
    printf("NOS " NOS_IDF_TEST_NAME " regression init chip=%d cores=%d\n", chip.model, chip.cores);
#ifdef NOS_IDF_ECOSYSTEM
    extern unsigned nos_esp_idf_ecosystem_link_count(void);
    assert(nos_esp_idf_ecosystem_link_count() == NOS_IDF_ECOSYSTEM_ENTRIES);
    puts("PASS " NOS_IDF_TEST_NAME "_sdk_ecosystem_linkage");
#if CONFIG_IDF_TARGET_ESP32C3
    static const enum nos_service_kind absent[] = {
        NOS_SERVICE_USB_HOST, NOS_SERVICE_USB_DEVICE, NOS_SERVICE_USB_OTG,
        NOS_SERVICE_MCPWM, NOS_SERVICE_COUNTER, NOS_SERVICE_SDMMC, NOS_SERVICE_DISPLAY};
    struct nos_service *unsupported;
    for (unsigned i = 0; i < sizeof(absent) / sizeof(absent[0]); i++) {
        struct nos_idf_device_config c = {.name = "test.absent", .kind = absent[i]};
        assert(nos_idf_device_create(&c, &unsupported) == -NOS_DRV_ENOTSUP && !unsupported);
    }
    struct nos_idf_device_config invalid = {
        .name = "test.invalid-i2c", .kind = NOS_SERVICE_I2C, .instance = 1,
        .hz = 100000, .pins = {4, 5}};
    assert(nos_idf_device_create(&invalid, &unsupported) == -NOS_DRV_EINVAL);
    invalid = (struct nos_idf_device_config){
        .name = "test.invalid-spi", .kind = NOS_SERVICE_SPI, .instance = 3,
        .hz = 1000000, .pins = {4, 5, 6, 7}};
    assert(nos_idf_device_create(&invalid, &unsupported) == -NOS_DRV_EINVAL);
    struct nos_service *pin, *serial;
    invalid = (struct nos_idf_device_config){
        .name = "test.usb-pin", .kind = NOS_SERVICE_GPIO, .pins = {19}};
    assert(!nos_idf_device_create(&invalid, &pin) && !nos_service_start(pin));
    invalid = (struct nos_idf_device_config){
        .name = "test.usb-serial", .kind = NOS_SERVICE_USB_SERIAL_JTAG};
    assert(!nos_idf_device_create(&invalid, &serial));
    assert(nos_service_start(serial) == -NOS_DRV_EBUSY);
    assert(!nos_idf_device_destroy(&serial) && !nos_idf_device_destroy(&pin));
    puts("PASS " NOS_IDF_TEST_NAME "_target_capability_limits");
#endif
#endif
    assert(xTaskGetSchedulerState() == taskSCHEDULER_RUNNING);
    main_handle = xTaskGetCurrentTaskHandle();
    done = xSemaphoreCreateCounting(8, 0);
    assert(done);
    assert(nos_syscall(NOS_SYS_GETPID, 0, 0, 0) >= 0);
    portMUX_TYPE native_mux = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&native_mux);
    assert(nos_syscall(NOS_SYS_GETPID, 0, 0, 0) == -NOS_EDEADLK);
    assert(nos_idf_devices_init() == -NOS_DRV_EBUSY);
    portEXIT_CRITICAL(&native_mux);
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
    char cwd[NOS_PATH_MAX];
    assert(nos_getcwd(cwd, sizeof(cwd)) >= 0 && !strcmp(cwd, "/"));
    assert(nos_syscall(NOS_SYS_SERVICE_RESTART, 0, 0, 0) == -NOS_ENOTSUP);
    puts("PASS " NOS_IDF_TEST_NAME "_native_syscalls");

    assert(xTaskCreate(spinner, "spin0", 2048, (void *)0, 1, NULL) == pdPASS);
    assert(xTaskCreate(spinner, "spin1", 2048, (void *)1, 1, NULL) == pdPASS);
    vTaskDelay(10);
    assert(spins[0] && spins[1]);
    stop_spinning = 1;
    wait_done(2);
    puts("PASS " NOS_IDF_TEST_NAME "_timer_preemption");

    queue = xQueueCreate(2, sizeof(uint32_t));
    assert(queue);
    assert(xTaskCreate(queue_consumer, "queue", 3072, NULL, 3, NULL) == pdPASS);
    uint32_t value = 0x12345678;
    assert(xQueueSend(queue, &value, 10) == pdTRUE);
    wait_done(1);
    vQueueDelete(queue);
    puts("PASS " NOS_IDF_TEST_NAME "_queue_timeout");

    errno = 7;
    assert(xTaskCreate(tls_worker, "tls1", 4096, (void *)1, 3, NULL) == pdPASS);
    assert(xTaskCreate(tls_worker, "tls2", 4096, (void *)2, 3, NULL) == pdPASS);
    wait_done(2);
    assert(errno == 7 && thread_cookie == 0 && tls_deleted == 2);
    puts("PASS " NOS_IDF_TEST_NAME "_tls_errno_" NOS_IDF_FP_TEST "_cleanup");

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
    puts("PASS " NOS_IDF_TEST_NAME "_event_stream");

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
    puts("PASS " NOS_IDF_TEST_NAME "_timers_notifications");

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
    puts("PASS " NOS_IDF_TEST_NAME "_native_ipc");

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
    puts("PASS " NOS_IDF_TEST_NAME "_priority_inheritance_native_exit");

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
    vTaskDelay(2); /* Retire self-deleted tasks before checking the full pool. */
    TaskHandle_t capacity[32];
    unsigned baseline_tasks = uxTaskGetNumberOfTasks();
    unsigned created = 0;
    size_t capacity_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    while (created < 32 &&
           xTaskCreate(passive, "limit", 2048, NULL, 0, &capacity[created]) == pdPASS)
        created++;
    assert(created > 0 && created < 32 && capacity[created] == NULL);
    assert(created + baseline_tasks == NOS_TASKS);
    for (unsigned i = 0; i < created; i++)
        vTaskDelete(capacity[i]);
    assert(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) == capacity_free);
    TaskHandle_t wrong_core = (void *)1;
    assert(xTaskCreatePinnedToCore(passive, "cpu1", 2048, NULL, 0, &wrong_core, 1) != pdPASS);
    assert(!wrong_core && heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) == capacity_free);
    puts("PASS " NOS_IDF_TEST_NAME "_static_tasks_capacity_rollback");

    assert(nos_idf_devices_init() == 0);
    extern void nos_esp_vfs_regression(void);
    nos_esp_vfs_regression();
    extern void nos_esp_libc_regression(void);
    nos_esp_libc_regression();
    struct nos_idf_device_config config = {
        .name = "test.gpio4", .kind = NOS_SERVICE_GPIO, .pins = {4, -1, -1, -1}};
    struct nos_service *gpio, *other, *uart, *storage, *ota;
    assert(nos_idf_device_create(&config, &gpio) == 0);
    assert(nos_service_start(gpio) == 0);
    portENTER_CRITICAL(&native_mux);
    int critical_level;
    assert(nos_service_call(gpio, NOS_IO_READ, &critical_level, sizeof(critical_level)) == -NOS_DRV_EBUSY);
    portEXIT_CRITICAL(&native_mux);
    struct nos_idf_device_config conflict = {
        .name = "test.i2c-pin-conflict", .kind = NOS_SERVICE_I2C, .pins = {4, 5}, .hz = 100000};
    assert(nos_idf_device_create(&conflict, &other) == 0);
    assert(nos_service_start(other) == -NOS_DRV_EBUSY);
    assert(nos_idf_device_destroy(&other) == 0);
    config.name = "test.duplicate-pin";
    assert(nos_idf_device_create(&config, &other) == 0);
    assert(nos_service_start(other) == -NOS_DRV_EBUSY);
    assert(nos_idf_device_destroy(&other) == 0 && !other);
    struct nos_gpio_config pins = {NOS_GPIO_INPUT, NOS_GPIO_PULL_DOWN, 0, 0};
    assert(nos_service_call(gpio, NOS_IO_CONFIGURE, &pins, sizeof(pins)) == 0);
    int level;
    assert(nos_service_call(gpio, NOS_IO_READ, &level, sizeof(level)) == 0);
    assert(level == 0 || level == 1);
    assert(nos_service_call(gpio, NOS_IO_READ, &level, 1) == -NOS_DRV_ENOTSUP);
    assert(nos_idf_device_destroy(&gpio) == 0 && !gpio);
    /* Boot console pins cannot be seized even in trusted shared memory. */
    config.name = "test.reserved-console";
    config.pins[0] = NOS_IDF_CONSOLE_PIN;
    assert(nos_idf_device_create(&config, &other) == 0);
    assert(nos_service_start(other) == -NOS_DRV_EBUSY);
    assert(nos_idf_device_destroy(&other) == 0);
    config = (struct nos_idf_device_config){.name = "test.uart1",
                                            .kind = NOS_SERVICE_UART,
                                            .instance = 1,
                                            .pins = {-1, -1, -1, -1},
                                            .hz = 115200};
    assert(nos_idf_device_create(&config, &uart) == 0);
    assert(nos_service_start(uart) == 0);
    uint8_t byte;
    struct nos_io_buffer buffer = {&byte, 1, 99};
    assert(nos_service_call(uart, NOS_IO_READ, &buffer, sizeof(buffer)) == -NOS_DRV_EAGAIN);
    assert(buffer.actual == 0);
    buffer = (struct nos_io_buffer){"NOS", 3, 0};
    assert(nos_service_call(uart, NOS_IO_WRITE, &buffer, sizeof(buffer)) == 0);
    assert(buffer.actual > 0 && buffer.actual <= 3);
    assert(nos_service_stop(uart) == 0);
    assert(nos_service_call(uart, NOS_IO_WRITE, &buffer, sizeof(buffer)) == -NOS_DRV_ENODEV);
    assert(nos_idf_device_destroy(&uart) == 0);
    config = (struct nos_idf_device_config){
        .name = "test.nvs", .kind = NOS_SERVICE_STORAGE, .storage_namespace = "nos-test"};
    assert(nos_idf_device_create(&config, &storage) == 0);
    assert(nos_service_start(storage) == 0);
    struct nos_io_kv kv = {"value", 0x12345678};
    assert(nos_service_call(storage, NOS_IO_WRITE, &kv, sizeof(kv)) == 0);
    assert(nos_service_call(storage, NOS_IO_COMMIT, NULL, 0) == 0);
    kv.value = 0;
    assert(nos_service_call(storage, NOS_IO_READ, &kv, sizeof(kv)) == 0 && kv.value == 0x12345678);
    assert(nos_service_call(storage, NOS_IO_ERASE, NULL, 0) == 0);
    assert(nos_service_call(storage, NOS_IO_COMMIT, NULL, 0) == 0);
    assert(nos_idf_device_destroy(&storage) == 0);
    struct nos_service_info devices[2];
    size_t device_count;
    assert(nos_idf_devices_snapshot(devices, 2, &device_count) == 0 && device_count == 2);
    assert(!strcmp(devices[0].name, "system.flash-psram"));
    assert(!strcmp(devices[1].name, "system.console") &&
           devices[1].kind == NOS_SERVICE_CONSOLE && devices[1].resources_owned);
    puts("PASS " NOS_IDF_TEST_NAME "_sdk_gpio_uart_flash_nvs");
    puts("PASS " NOS_IDF_TEST_NAME "_managed_devices");
    config = (struct nos_idf_device_config){.name = "test.ota", .kind = NOS_SERVICE_OTA};
    assert(nos_idf_device_create(&config, &ota) == 0);
    assert(nos_service_start(ota) == 0);
    struct nos_io_ota_begin begin = {4096};
    assert(nos_service_call(ota, NOS_IO_OTA_BEGIN, &begin, sizeof(begin)) == 0);
    uint8_t invalid_image[32] = {0};
    buffer = (struct nos_io_buffer){invalid_image, sizeof(invalid_image), 0};
    assert(nos_service_call(ota, NOS_IO_WRITE, &buffer, sizeof(buffer)) == -NOS_DRV_EIO);
    assert(buffer.actual == 0);
    assert(nos_service_call(ota, NOS_IO_OTA_ABORT, NULL, 0) == 0);
    assert(nos_idf_device_destroy(&ota) == 0);
    puts("PASS " NOS_IDF_TEST_NAME "_sdk_ota_reject_abort");

    size_t device_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    struct nos_service *slots[32] = {0};
    char names[32][24];
    config = (struct nos_idf_device_config){.kind = NOS_SERVICE_GPIO, .pins = {4}};
    unsigned available_devices = 32 - device_count;
    for (unsigned i = 0; i < available_devices; i++) {
        snprintf(names[i], sizeof(names[i]), "capacity.%u", i);
        config.name = names[i];
        assert(nos_idf_device_create(&config, &slots[i]) == 0);
    }
    config.name = "capacity.full";
    assert(nos_idf_device_create(&config, &slots[available_devices]) == -NOS_DRV_ENOSPC &&
           !slots[available_devices]);
    for (unsigned i = 0; i < available_devices; i++)
        assert(nos_idf_device_destroy(&slots[i]) == 0 && !slots[i]);
    assert(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) == device_heap);
    puts("PASS " NOS_IDF_TEST_NAME "_device_capacity_no_leak");
#if defined(NOS_IDF_ECOSYSTEM) && !CONFIG_IDF_TARGET_ESP32C3
    struct nos_idf_usb_otg_config otg_board = {.id_pin = -1,
                                               .vbus_sense_pin = 4,
                                               .vbus_enable_pin = -1,
                                               .overcurrent_pin = -1,
                                               .allowed = NOS_USB_ALLOW_DEVICE};
    config = (struct nos_idf_device_config){
        .name = "test.otg", .kind = NOS_SERVICE_USB_OTG, .usb_otg = &otg_board};
    struct nos_service *otg_service;
    assert(!nos_idf_device_create(&config, &otg_service));
    assert(!nos_service_start(otg_service));
    struct nos_idf_device_config serial_config = {
        .name = "test.otg-serial-conflict", .kind = NOS_SERVICE_USB_SERIAL_JTAG};
    struct nos_service *serial_service;
    assert(!nos_idf_device_create(&serial_config, &serial_service));
    assert(nos_service_start(serial_service) == -NOS_DRV_EBUSY);
    assert(!nos_idf_device_destroy(&serial_service));
    enum nos_usb_role auto_role = NOS_USB_ROLE_AUTO;
    assert(!nos_service_call(otg_service, NOS_IO_USB_ROLE, &auto_role, sizeof(auto_role)));
    vTaskDelay(5);
    struct nos_usb_role_status role_status;
    assert(
        !nos_service_call(otg_service, NOS_IO_USB_OTG_STATUS, &role_status, sizeof(role_status)));
    assert(role_status.active == NOS_USB_ROLE_NONE && !role_status.sourcing);
    assert(!nos_idf_device_destroy(&otg_service));
    puts("PASS " NOS_IDF_TEST_NAME "_managed_otg_idle_worker");
#endif

    size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    for (unsigned i = 0; i < 40; i++) {
        assert(xTaskCreate(short_lived, "recycle", 3072, NULL, 3, NULL) == pdPASS);
        wait_done(1);
    }
    assert(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) == before);
    puts("PASS " NOS_IDF_TEST_NAME "_task_recycle_no_stack_leak");
    extern void nos_esp_storage_regression(void);
    nos_esp_storage_regression();
    puts("PASS " NOS_IDF_TEST_NAME "_nos_idf_regression");
    fflush(stdout);
}
