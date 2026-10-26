/* Copyright (C) 2026 NOS contributors. Deterministic scheduler contracts. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/list.h"
#include "freertos/timers.h"
#include "esp_timer.h"
#include "esp_idf_test.h"
#include "esp32_timing.h"
extern void vTaskPlaceOnEventList(List_t *, TickType_t);
static volatile unsigned release_idle, callbacks;
static TaskHandle_t blocker;
static SemaphoreHandle_t completed, cleaned;
static StaticSemaphore_t completed_storage, cleaned_storage;
static __thread unsigned cookie;
static volatile unsigned timer_calls;
static void timer_callback(TimerHandle_t timer) {
    timer_calls++;
    xSemaphoreGive((SemaphoreHandle_t)pvTimerGetTimerID(timer));
}
static void timer_stress(void) {
    static StaticSemaphore_t signal_storage;
    SemaphoreHandle_t signal = xSemaphoreCreateBinaryStatic(&signal_storage);
    TimerHandle_t timer = xTimerCreate("resume-stress", 2, pdFALSE, signal, timer_callback);
    assert(signal && timer);
    for (unsigned round = 0; round < 64; round++) {
        vTaskSuspendAll();
        vTaskSuspendAll();
        TickType_t frozen = xTaskGetTickCount();
        /* Starting a timer may yield to its daemon. Arm it only after the
         * scheduler is locked; otherwise a short timer may legally fire
         * before this test even enters the suspended interval. */
        assert(xTimerStart(timer, 0) == pdPASS);
        int64_t until = esp_timer_get_time() + 35000;
        while (esp_timer_get_time() < until) {
        }
        assert(xTaskGetTickCount() == frozen && timer_calls == round);
        assert(xTaskResumeAll() == pdFALSE && xTaskGetTickCount() == frozen);
        xTaskResumeAll();
        BaseType_t ready = xSemaphoreTake(signal, pdMS_TO_TICKS(3000));
        if (ready != pdTRUE)
            nos_esp_timing_dump("resume-timer-timeout");
        assert(ready == pdTRUE && timer_calls == round + 1);
        assert(xSemaphoreTake(signal, 0) == pdFALSE);
    }
    assert(xTimerDelete(timer, 10) == pdPASS);
    puts("PASS " NOS_IDF_TEST_NAME "_resume_timer_64_deadlines");
}
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
        printf(" task=%s id=%u state=%u wait=%u deleting=%u deadline=%llu\n", t->name,
               (unsigned)t->id, (unsigned)t->state, (unsigned)t->waiting, (unsigned)t->deleting,
               (unsigned long long)t->deadline);
    }
}
void nos_esp_timing_wait_reaped(uintptr_t handle, uint32_t start) {
    while (!nos_esp_timing_reaped(handle)) {
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= pdMS_TO_TICKS(3000))
            nos_esp_timing_dump("reap-timeout");
        assert(elapsed < pdMS_TO_TICKS(3000));
        vTaskDelay(1);
    }
}
void nos_esp_timing_regression(void) {
    /* IDF's first systimer ISR establishes its hardware counter baseline
     * and reports only one tick, even if delivery was late. Start the
     * suspended-tick contract after that ISR, rather than interpreting
     * 50 ms since boot as proof that at least two ticks were delivered. */
    TickType_t initial = xTaskGetTickCount();
    vTaskDelay(1);
    TickType_t ready = xTaskGetTickCount();
    assert((TickType_t)(ready - initial) >= 1);
    printf("PROBE systick_ready initial=%u ready=%u\n", (unsigned)initial, (unsigned)ready);
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
    completed = xSemaphoreCreateBinaryStatic(&completed_storage);
    cleaned = xSemaphoreCreateBinaryStatic(&cleaned_storage);
    assert(completed && cleaned);
    assert(xTaskCreate(busy, "idle-blocker", 3072, NULL, 1, &blocker) == pdPASS);
    errno = 73;
    TickType_t reap_start = xTaskGetTickCount();
    TaskHandle_t task;
    assert(xTaskCreate(worker, "reap-worker", 4096, NULL, 4, &task) == pdPASS);
    TickType_t deadline = reap_start + pdMS_TO_TICKS(3000);
    assert(xSemaphoreTake(completed, pdMS_TO_TICKS(3000)) == pdTRUE);
    vTaskDelay(2);
    int observed_errno = errno;
    unsigned observed_cookie = cookie, observed_callbacks = callbacks;
    release_idle = 1;
    assert(observed_errno == 73 && !observed_cookie && !observed_callbacks);
    TickType_t remaining = deadline - xTaskGetTickCount();
    assert((int32_t)remaining > 0 && xSemaphoreTake(cleaned, remaining) == pdTRUE);
    assert(callbacks == 1);
    nos_esp_timing_wait_reaped((uintptr_t)task, reap_start);
    nos_esp_timing_wait_reaped((uintptr_t)blocker, reap_start);
    /* Idle can be preempted inside Give(); static objects outlive both tasks. */
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
    timer_stress();
}
