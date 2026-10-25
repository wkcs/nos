/* Copyright (C) 2026 NOS contributors. Deterministic scheduler contracts. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/list.h"
#include "esp_timer.h"
#include "esp_idf_test.h"
#include "esp32_timing.h"
extern void vTaskPlaceOnEventList(List_t *, TickType_t);
static volatile unsigned release_idle, callbacks;
static TaskHandle_t blocker;
static SemaphoreHandle_t completed, cleaned;
static __thread unsigned cookie;
static void cleanup(int index, void *pointer) {
    assert(index == 0 && pointer == &callbacks);
    callbacks++;
    xSemaphoreGive(cleaned);
}
static void busy(void *unused) {
    (void)unused;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (!release_idle)
        __asm__ volatile("" ::: "memory");
}
static void worker(void *unused) {
    (void)unused;
    cookie = 123;
    errno = 29;
    vTaskSetThreadLocalStoragePointerAndDelCallback(NULL, 0, (void *)&callbacks, cleanup);
    xTaskNotifyGive(blocker);
    xSemaphoreGive(completed);
}
void nos_esp_timing_dump(const char *stage) {
    struct nos_timing_snapshot s;
    nos_esp_timing_snapshot(&s);
    printf("TIMING %s ticks=%llu deferred=%llu suspended=%u\n", stage, (unsigned long long)s.ticks,
           (unsigned long long)s.deferred, (unsigned)s.suspended);
    for (unsigned i = 0; i < s.count; i++) {
        struct nos_timing_task *t = &s.tasks[i];
        printf(" task=%s id=%u state=%u wait=%u deleting=%u deadline=%llu\n", t->name, (unsigned)t->id,
               (unsigned)t->state, (unsigned)t->waiting, (unsigned)t->deleting, (unsigned long long)t->deadline);
    }
}
void nos_esp_timing_regression(void) {
    List_t event;
    vListInitialise(&event);
    vTaskSuspendAll();
    vTaskSuspendAll();
    TickType_t before = xTaskGetTickCount();
    vTaskPlaceOnEventList(&event, 2);
    int64_t until = esp_timer_get_time() + 50000;
    while (esp_timer_get_time() < until) {
    }
    TickType_t during = xTaskGetTickCount();
    unsigned linked = listCURRENT_LIST_LENGTH(&event);
    assert(xTaskResumeAll() == pdFALSE);
    TickType_t nested = xTaskGetTickCount();
    struct nos_timing_snapshot frozen;
    nos_esp_timing_snapshot(&frozen);
    xTaskResumeAll();
    TickType_t after = xTaskGetTickCount();
    printf("PROBE suspend before=%u during=%u nested=%u after=%u linked=%u deferred=%llu\n",
           (unsigned)before, (unsigned)during, (unsigned)nested, (unsigned)after, linked,
           (unsigned long long)frozen.deferred);
    assert(listLIST_IS_EMPTY(&event));
    UBaseType_t priority = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, 3);
    completed = xSemaphoreCreateBinary();
    cleaned = xSemaphoreCreateBinary();
    assert(completed && cleaned);
    assert(xTaskCreate(busy, "idle-blocker", 3072, NULL, 1, &blocker) == pdPASS);
    errno = 73;
    assert(xTaskCreate(worker, "reap-worker", 4096, NULL, 4, NULL) == pdPASS);
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
    assert(xSemaphoreTake(completed, pdMS_TO_TICKS(3000)) == pdTRUE);
    vTaskDelay(2);
    int observed_errno = errno;
    unsigned observed_cookie = cookie, observed_callbacks = callbacks;
    release_idle = 1;
    assert(observed_errno == 73 && !observed_cookie && !observed_callbacks);
    TickType_t remaining = deadline - xTaskGetTickCount();
    assert((int32_t)remaining > 0 && xSemaphoreTake(cleaned, remaining) == pdTRUE);
    assert(callbacks == 1);
    vSemaphoreDelete(completed);
    vSemaphoreDelete(cleaned);
    vTaskPrioritySet(NULL, priority);
    vTaskDelay(2);
    printf("PROBE async_reclaim fixed_two_ticks=%u completed=%u errno=%d cookie=%u\n",
           observed_callbacks, callbacks, observed_errno, observed_cookie);
    puts("PASS " NOS_IDF_TEST_NAME "_async_reclaim_completion_contract");
    if (during != before || nested != before || linked != 1)
        nos_esp_timing_dump("suspend-contract-failed");
    assert(during == before && nested == before && linked == 1);
    assert(frozen.deferred >= 2 && (TickType_t)(after - before) >= frozen.deferred);
    puts("PASS " NOS_IDF_TEST_NAME "_suspended_ticks_event_list_atomicity");
}
