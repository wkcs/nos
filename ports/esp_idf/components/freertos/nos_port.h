/* Copyright (C) 2026 NOS contributors. */
#ifndef NOS_ESP_IDF_PORT_H
#define NOS_ESP_IDF_PORT_H
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/list.h"
#include "kernel.h"
struct nos_idf_task {
    StackType_t *top, *stack_end;
    ListItem_t event;
    struct _reent reent;
    struct task *native;
    void *tls[configNUM_THREAD_LOCAL_STORAGE_POINTERS];
    void (*tls_delete[CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS])(int, void *);
    char name[configMAX_TASK_NAME_LEN];
    uint32_t notify[configTASK_NOTIFICATION_ARRAY_ENTRIES];
    uint8_t notify_state[configTASK_NOTIFICATION_ARRAY_ENTRIES];
    UBaseType_t mutexes, initial_priority;
    unsigned inherited_priority, ceiling_priority;
    StaticTask_t *static_buffer;
    uint8_t waiting, suspended, pending, static_storage, deleting, delay_aborted;
    TaskFunction_t entry;
    void *argument;
    uint32_t stack_bytes;
    StackType_t *stack;
};
_Static_assert(offsetof(struct nos_idf_task, top) == 0 &&
                   offsetof(struct nos_idf_task, stack_end) == 4,
               "IDF SDK context ABI");
void nos_idf_flush_pending(void);
void nos_idf_scheduler_ready(void);
void nos_idf_idle(void);
intptr_t nos_idf_syscall(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
#endif
