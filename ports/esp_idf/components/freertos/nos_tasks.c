/* Copyright (C) 2026 NOS contributors. ESP-IDF task API over native NOS tasks.
 * ESP-IDF's list/queue/timer/event/stream implementations are compatibility
 * collections; no FreeRTOS tasks.c or FreeRTOS scheduler is linked. */
#include <string.h>
#include "nos_port.h"
#include "esp_heap_caps.h"
#include "esp_freertos_hooks.h"
#include "freertos/timers.h"
#include "esp_attr.h"
#include "esp_newlib.h"
#ifdef NOS_XTENSA
#include "xtensa_context.h"
#endif
static struct nos_idf_task records[NOS_TASKS];
void *pxCurrentTCBs[1];
static unsigned started, suspended, pended_yield;
static portMUX_TYPE api_mux = portMUX_INITIALIZER_UNLOCKED;
const DRAM_ATTR uint32_t offset_pxEndOfStack = offsetof(struct nos_idf_task, stack_end);
extern int nos_idf_kernel_initialized(void);
extern void vPortYield(void);
static struct nos_idf_task *self(void)
{
    return pxCurrentTCBs[0];
}
static struct nos_idf_task *lookup(TaskHandle_t handle)
{
    if (!handle)
        return self();
    uintptr_t token = (uintptr_t)handle;
    uintptr_t base = (uintptr_t)records;
    if (token >= base && token - base < sizeof(records) &&
        (token - base) % sizeof(records[0]) == 0) {
        struct nos_idf_task *r = &records[(token - base) / sizeof(records[0])];
        return r->native && !r->deleting ? r : NULL;
    }
    if (token < 0x10000)
        return NULL;
    struct task *t = find_task(token - 0x10000);
    if (!t) {
        unsigned slot = token - 0x10000;
        return slot < NOS_TASKS && records[slot].pending ? &records[slot] : NULL;
    }
    struct nos_idf_task *r = &records[t->id % NOS_TASKS];
    return r->native == t && !r->deleting ? r : NULL;
}
static TaskHandle_t handle(struct nos_idf_task *r)
{
    return r && r->native ? (TaskHandle_t)(uintptr_t)(0x10000 + r->native->id) : NULL;
}
static void reclaim(struct nos_idf_task *r);
static unsigned priority(UBaseType_t p)
{
    return 31u - (unsigned)p;
}
struct _reent *__getreent(void)
{
    return started && self() ? &self()->reent : _GLOBAL_REENT;
}
static void idf_entry(uintptr_t pointer, uintptr_t unused)
{
    (void)unused;
    struct nos_idf_task *r = (void *)pointer;
    r->entry(r->argument);
    vTaskDelete(NULL);
    abort();
}
static int activate(unsigned slot)
{
    struct nos_idf_task *r = &records[slot];
    int id = create_task(slot, (uintptr_t)r->entry, (uintptr_t)r->argument,
                         priority(r->initial_priority),
                         NOS_CAP_USER | NOS_CAP_PREEMPT | NOS_CAP_IPC, UINT32_MAX);
    if (id < 0)
        return id;
    r->native = &tasks[slot];
    r->inherited_priority = r->ceiling_priority = 31;
    memset(r->stack, 0xa5, r->stack_bytes);
    arch_frame_init(&r->native->frame, (uintptr_t)idf_entry,
                    ((uintptr_t)r->stack + r->stack_bytes) & ~15u, (uintptr_t)r, 0);
    r->stack_end = (StackType_t *)r->native->frame.stack_end;
    r->top = (StackType_t *)r->native->frame.context;
    vListInitialiseItem(&r->event);
    listSET_LIST_ITEM_OWNER(&r->event, r);
    esp_reent_init(&r->reent);
    r->pending = 0;
    return 0;
}
static BaseType_t create(TaskFunction_t function, const char *name, uint32_t bytes, void *arg,
                         UBaseType_t prio, TaskHandle_t *out, BaseType_t core, StackType_t *stack,
                         StaticTask_t *buffer, uint32_t memory_caps)
{
    if (out)
        *out = NULL;
    if (!function || prio >= configMAX_PRIORITIES || (core != 0 && core != tskNO_AFFINITY) ||
        bytes < 1024 || (stack && !xPortcheckValidStackMem(stack)) || xPortInIsrContext())
        return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
    unsigned owns_stack = !stack;
    if (owns_stack) {
        stack = heap_caps_malloc(bytes, memory_caps);
        if (!stack)
            return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
    }
    portENTER_CRITICAL(&api_mux);
    unsigned slot;
    for (slot = 0; slot < NOS_IDLE_FIRST; slot++)
        if (!records[slot].pending && !records[slot].native && !records[slot].deleting &&
            tasks[slot].state == TASK_UNUSED)
            break;
    if (slot == NOS_IDLE_FIRST) {
        portEXIT_CRITICAL(&api_mux);
        if (owns_stack)
            heap_caps_free(stack);
        return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
    }
    struct nos_idf_task *r = &records[slot];
    *r = (struct nos_idf_task){.entry = function,
                               .argument = arg,
                               .stack_bytes = bytes,
                               .stack = stack,
                               .pending = 1,
                               .initial_priority = prio,
                               .static_storage = !owns_stack,
                               .static_buffer = buffer};
    if (name)
        strlcpy(r->name, name, sizeof(r->name));
    if (nos_idf_kernel_initialized() && activate(slot)) {
        memset(r, 0, sizeof(*r));
        portEXIT_CRITICAL(&api_mux);
        if (owns_stack)
            heap_caps_free(stack);
        return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
    }
    if (out)
        *out = r->native ? handle(r) : (TaskHandle_t)(uintptr_t)(0x10000 + slot);
    portEXIT_CRITICAL(&api_mux);
    if (started && !suspended)
        vPortYield();
    return pdPASS;
}
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t f, const char *n, const uint32_t s, void *a,
                                   UBaseType_t p, TaskHandle_t *h, const BaseType_t c)
{
    return create(f, n, s, a, p, h, c, NULL, NULL, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t f, const char *n, const uint32_t s,
                                           void *a, UBaseType_t p, StackType_t *stack,
                                           StaticTask_t *buffer, const BaseType_t core)
{
    TaskHandle_t h = NULL;
    if (!stack || !buffer)
        return NULL;
    return create(f, n, s, a, p, &h, core, stack, buffer, 0) == pdPASS ? h : NULL;
}
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t f, const char *n,
                                           const configSTACK_DEPTH_TYPE s, void *a, UBaseType_t p,
                                           TaskHandle_t *h, const BaseType_t c, UBaseType_t caps)
{
    return create(f, n, s, a, p, h, c, NULL, NULL, caps | MALLOC_CAP_8BIT);
}
void vTaskDeleteWithCaps(TaskHandle_t h)
{
    vTaskDelete(h);
}
void nos_idf_flush_pending(void)
{
    for (unsigned i = 0; i < NOS_IDLE_FIRST; i++)
        if (records[i].pending)
            configASSERT(activate(i) == 0);
    struct nos_idf_task *idle = &records[NOS_IDLE_FIRST];
    idle->native = &tasks[NOS_IDLE_FIRST];
    idle->top = (StackType_t *)idle->native->frame.context;
    idle->stack_end = (StackType_t *)idle->native->frame.stack_end;
    idle->stack = (StackType_t *)idle->native->memory;
    idle->stack_bytes = USER_BYTES;
    idle->static_storage = 1;
    idle->inherited_priority = idle->ceiling_priority = 31;
    strlcpy(idle->name, "IDLE0", sizeof(idle->name));
    vListInitialiseItem(&idle->event);
    listSET_LIST_ITEM_OWNER(&idle->event, idle);
    esp_reent_init(&idle->reent);
    configASSERT(xTimerCreateTimerTask() == pdPASS);
}
void nos_idf_scheduler_ready(void)
{
    struct nos_idf_task *r = &records[current_task->id % NOS_TASKS];
    if (!r->native) {
        esp_reent_init(&r->reent);
        r->native = current_task;
        r->top = (StackType_t *)current_task->frame.context;
        r->stack_end = (StackType_t *)current_task->frame.stack_end;
        vListInitialiseItem(&r->event);
    }
    pxCurrentTCBs[0] = r;
    started = 1;
}
void vTaskStartScheduler(void)
{
    kernel_start();
}
void vTaskEndScheduler(void)
{
    kernel_fail("IDF scheduler cannot end");
}
void vTaskSwitchContext(void)
{
    uintptr_t irq = arch_irq_save();
    struct nos_idf_task *previous = self();
    if (suspended) {
        pended_yield = 1;
        arch_irq_restore(irq);
        return;
    }
    if (previous && previous->native && !previous->deleting)
        previous->native->frame.context = (uintptr_t)previous->top;
    if (current_task)
        current_task->run_cpu = UINT32_MAX;
    struct task *next = select_task();
    configASSERT(next);
    current_task = next;
    next->run_cpu = 0;
    struct nos_idf_task *r = &records[next->id % NOS_TASKS];
    if (!r->native) {
        esp_reent_init(&r->reent);
        r->native = next;
        r->stack_end = (StackType_t *)next->frame.stack_end;
        vListInitialiseItem(&r->event);
    }
    r->top = (StackType_t *)next->frame.context;
    pxCurrentTCBs[0] = r;
    arch_irq_restore(irq);
}
BaseType_t xTaskIncrementTick(void)
{
    uintptr_t irq = arch_irq_save();
    scheduler_tick();
    for (unsigned i = 0; i < NOS_TASKS; i++) {
        struct nos_idf_task *r = &records[i];
        if (r->native && r->waiting && r->native->state == TASK_READY) {
            if (listLIST_ITEM_CONTAINER(&r->event))
                uxListRemove(&r->event);
            r->waiting = 0;
        }
    }
    arch_irq_restore(irq);
    if (suspended)
        pended_yield = 1;
    return suspended ? pdFALSE : pdTRUE;
}
TickType_t xTaskGetTickCount(void)
{
    return (TickType_t)kernel_ticks;
}
TickType_t xTaskGetTickCountFromISR(void)
{
    return (TickType_t)kernel_ticks;
}
BaseType_t xTaskGetSchedulerState(void)
{
    return !started    ? taskSCHEDULER_NOT_STARTED
           : suspended ? taskSCHEDULER_SUSPENDED
                       : taskSCHEDULER_RUNNING;
}
TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return handle(self());
}
TaskHandle_t xTaskGetCurrentTaskHandleForCore(BaseType_t c)
{
    return c == 0 ? handle(self()) : NULL;
}
TaskHandle_t xTaskGetIdleTaskHandleForCore(BaseType_t c)
{
    return c == 0 ? handle(&records[NOS_IDLE_FIRST]) : NULL;
}
TaskHandle_t xTaskGetIdleTaskHandle(void)
{
    return xTaskGetIdleTaskHandleForCore(0);
}
BaseType_t xTaskGetCoreID(TaskHandle_t task)
{
    return lookup(task) ? 0 : tskNO_AFFINITY;
}
UBaseType_t uxTaskGetNumberOfTasks(void)
{
    UBaseType_t n = 0;
    for (unsigned i = 0; i < NOS_TASKS; i++)
        if (tasks[i].state != TASK_UNUSED && tasks[i].state != TASK_RETIRED)
            n++;
    return n;
}
char *pcTaskGetName(TaskHandle_t h)
{
    struct nos_idf_task *r = lookup(h);
    return r ? r->name : "bootstrap";
}
UBaseType_t uxTaskPriorityGet(TaskHandle_t h)
{
    struct nos_idf_task *r = lookup(h);
    return r ? 31 - r->native->priority : 0;
}
UBaseType_t uxTaskPriorityGetFromISR(TaskHandle_t h)
{
    return uxTaskPriorityGet(h);
}
static void recompute_priority(struct nos_idf_task *r)
{
    r->native->compat_priority =
        r->inherited_priority < r->ceiling_priority ? r->inherited_priority : r->ceiling_priority;
    mutex_recompute();
    if (!(listGET_LIST_ITEM_VALUE(&r->event) & 0x80000000u)) {
        List_t *list = listLIST_ITEM_CONTAINER(&r->event);
        if (list)
            uxListRemove(&r->event);
        listSET_LIST_ITEM_VALUE(&r->event, configMAX_PRIORITIES - (31 - r->native->priority));
        if (list)
            vListInsert(list, &r->event);
    }
}
void vTaskPrioritySet(TaskHandle_t h, UBaseType_t p)
{
    uintptr_t irq = arch_irq_save();
    struct nos_idf_task *r = lookup(h);
    configASSERT(r && p < configMAX_PRIORITIES);
    r->native->base_priority = priority(p);
    recompute_priority(r);
    arch_irq_restore(irq);
    if (started && !suspended)
        vPortYield();
}
void vTaskDelay(TickType_t ticks)
{
    if (!ticks) {
        vPortYield();
        return;
    }
    configASSERT(started && !suspended);
    uintptr_t irq = arch_irq_save();
    current_task->wake_tick = kernel_ticks + ticks;
    current_task->state = TASK_SLEEP;
    arch_irq_restore(irq);
    vPortYield();
}
BaseType_t xTaskDelayUntil(TickType_t *previous, const TickType_t increment)
{
    configASSERT(previous && increment);
    TickType_t before = *previous, target = before + increment, now = xTaskGetTickCount();
    BaseType_t delay =
        now < before ? (target < before && target > now) : (target < before || target > now);
    *previous = target;
    if (delay)
        vTaskDelay(target - now);
    return delay;
}
void vTaskSuspendAll(void)
{
    portENTER_CRITICAL(&api_mux);
    suspended++;
    portEXIT_CRITICAL(&api_mux);
}
BaseType_t xTaskResumeAll(void)
{
    portENTER_CRITICAL(&api_mux);
    configASSERT(suspended);
    suspended--;
    unsigned y = !suspended && pended_yield;
    if (!suspended)
        pended_yield = 0;
    portEXIT_CRITICAL(&api_mux);
    if (y)
        vPortYield();
    return y ? pdTRUE : pdFALSE;
}
void vTaskMissedYield(void)
{
    pended_yield = 1;
}
void vTaskSuspend(TaskHandle_t h)
{
    struct nos_idf_task *r = lookup(h);
    configASSERT(r);
    uintptr_t irq = arch_irq_save();
    if (listLIST_ITEM_CONTAINER(&r->event))
        uxListRemove(&r->event);
    r->waiting = 0;
    r->suspended = 1;
    r->native->wake_tick = UINT64_MAX;
    r->native->state = TASK_SLEEP;
    arch_irq_restore(irq);
    if (r == self())
        vPortYield();
}
void vTaskResume(TaskHandle_t h)
{
    configASSERT(h);
    BaseType_t higher = xTaskResumeFromISR(h);
    if (higher && started && !suspended)
        vPortYield();
}
BaseType_t xTaskResumeFromISR(TaskHandle_t h)
{
    uintptr_t irq = arch_irq_save();
    struct nos_idf_task *r = lookup(h);
    BaseType_t higher = pdFALSE;
    if (r && r->suspended) {
        r->suspended = 0;
        r->native->state = TASK_READY;
        higher = !self() || r->native->priority <= current_task->priority;
        if (higher)
            pended_yield = 1;
    }
    arch_irq_restore(irq);
    return higher;
}
void vTaskDelete(TaskHandle_t h)
{
    struct nos_idf_task *r = lookup(h);
    configASSERT(r);
    uintptr_t irq = arch_irq_save();
    if (listLIST_ITEM_CONTAINER(&r->event))
        uxListRemove(&r->event);
    task_finish(r->native, 0);
    unsigned current = r == self();
    r->deleting = current ? 1 : 2;
    arch_irq_restore(irq);
    if (!current) {
        reclaim(r);
        if (started && !suspended)
            vPortYield();
    }
    if (current) {
        vPortYield();
        abort();
    }
}
void vTaskPlaceOnEventList(List_t *list, const TickType_t ticks)
{
    struct nos_idf_task *r = self();
    configASSERT(r);
    listSET_LIST_ITEM_VALUE(&r->event, configMAX_PRIORITIES - uxTaskPriorityGet(NULL));
    vListInsert(list, &r->event);
    r->waiting = 1;
    r->native->wake_tick = ticks == portMAX_DELAY ? UINT64_MAX : kernel_ticks + ticks;
    r->native->state = TASK_SLEEP;
}
void vTaskPlaceOnEventListRestricted(List_t *list, TickType_t ticks, const BaseType_t indefinite)
{
    vTaskPlaceOnEventList(list, indefinite ? portMAX_DELAY : ticks);
}
void vTaskPlaceOnUnorderedEventList(List_t *list, const TickType_t value, const TickType_t ticks)
{
    struct nos_idf_task *r = self();
    listSET_LIST_ITEM_VALUE(&r->event, value | 0x80000000u);
    vListInsertEnd(list, &r->event);
    r->waiting = 1;
    r->native->wake_tick = ticks == portMAX_DELAY ? UINT64_MAX : kernel_ticks + ticks;
    r->native->state = TASK_SLEEP;
}
BaseType_t xTaskRemoveFromEventList(const List_t *list)
{
    if (listLIST_IS_EMPTY(list))
        return pdFALSE;
    struct nos_idf_task *r = listGET_OWNER_OF_HEAD_ENTRY(list);
    uxListRemove(&r->event);
    r->waiting = 0;
    r->native->state = TASK_READY;
    pended_yield = 1;
    return pdTRUE;
}
void vTaskRemoveFromUnorderedEventList(ListItem_t *item, const TickType_t value)
{
    struct nos_idf_task *r = listGET_LIST_ITEM_OWNER(item);
    uxListRemove(item);
    listSET_LIST_ITEM_VALUE(item, value | 0x80000000u);
    r->waiting = 0;
    r->native->state = TASK_READY;
    pended_yield = 1;
}
TickType_t uxTaskResetEventItemValue(void)
{
    struct nos_idf_task *r = self();
    TickType_t value = listGET_LIST_ITEM_VALUE(&r->event);
    listSET_LIST_ITEM_VALUE(&r->event, configMAX_PRIORITIES - uxTaskPriorityGet(NULL));
    return value;
}
void vTaskInternalSetTimeOutState(TimeOut_t *t)
{
    t->xOverflowCount = (BaseType_t)(kernel_ticks >> 32);
    t->xTimeOnEntering = xTaskGetTickCount();
}
void vTaskSetTimeOutState(TimeOut_t *t)
{
    uintptr_t irq = arch_irq_save();
    vTaskInternalSetTimeOutState(t);
    arch_irq_restore(irq);
}
BaseType_t xTaskCheckForTimeOut(TimeOut_t *t, TickType_t *wait)
{
    if (self()->delay_aborted) {
        self()->delay_aborted = 0;
        return pdTRUE;
    }
    if (*wait == portMAX_DELAY)
        return pdFALSE;
    uint64_t old = ((uint64_t)(uint32_t)t->xOverflowCount << 32) | t->xTimeOnEntering;
    uint64_t elapsed = kernel_ticks - old;
    if (elapsed >= *wait) {
        *wait = 0;
        return pdTRUE;
    }
    *wait -= (TickType_t)elapsed;
    vTaskInternalSetTimeOutState(t);
    return pdFALSE;
}
TaskHandle_t pvTaskIncrementMutexHeldCount(void)
{
    struct nos_idf_task *r = self();
    if (r)
        r->mutexes++;
    return handle(r);
}
BaseType_t xTaskPriorityInherit(TaskHandle_t h)
{
    if (!h)
        return pdFALSE;
    struct nos_idf_task *r = lookup(h);
    if (!r || !self())
        return pdFALSE;
    unsigned p = current_task->priority;
    BaseType_t inherited = r->native->base_priority > p;
    if (r->inherited_priority > p) {
        r->inherited_priority = p;
        recompute_priority(r);
    }
    return inherited;
}
BaseType_t xTaskPriorityDisinherit(TaskHandle_t h)
{
    if (!h)
        return pdFALSE;
    struct nos_idf_task *r = lookup(h);
    if (!r)
        return pdFALSE;
    configASSERT(r->mutexes);
    r->mutexes--;
    if (r->mutexes)
        return pdFALSE;
    unsigned old = r->native->priority;
    r->inherited_priority = 31;
    recompute_priority(r);
    return old != r->native->priority;
}
void vTaskPriorityDisinheritAfterTimeout(TaskHandle_t h, UBaseType_t p)
{
    struct nos_idf_task *r = lookup(h);
    if (r && r->mutexes == 1) {
        r->inherited_priority = priority(p);
        recompute_priority(r);
    }
}
void vTaskSetThreadLocalStoragePointer(TaskHandle_t h, BaseType_t index, void *value)
{
    struct nos_idf_task *r = lookup(h);
    configASSERT(r && index >= 0 && index < configNUM_THREAD_LOCAL_STORAGE_POINTERS);
    r->tls[index] = value;
}
void *pvTaskGetThreadLocalStoragePointer(TaskHandle_t h, BaseType_t index)
{
    struct nos_idf_task *r = lookup(h);
    return r && index >= 0 && index < configNUM_THREAD_LOCAL_STORAGE_POINTERS ? r->tls[index]
                                                                              : NULL;
}
#if CONFIG_FREERTOS_TLSP_DELETION_CALLBACKS
void vTaskSetThreadLocalStoragePointerAndDelCallback(TaskHandle_t h, BaseType_t i, void *v,
                                                     TlsDeleteCallbackFunction_t cb)
{
    configASSERT(i >= 0 && i < CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS);
    vTaskSetThreadLocalStoragePointer(h, i, v);
    lookup(h)->tls_delete[i] = cb;
}

#endif

/* Notification state belongs to the task; IRQ masking makes test/block/wake
 * atomic on the supported single-core profile. The NOS timer owns timeouts. */
BaseType_t xTaskGenericNotifyFromISR(TaskHandle_t h, UBaseType_t i, uint32_t value,
                                     eNotifyAction action, uint32_t *previous, BaseType_t *higher)
{
    uintptr_t irq = arch_irq_save();
    struct nos_idf_task *r = lookup(h);
    configASSERT(r && i < configTASK_NOTIFICATION_ARRAY_ENTRIES);
    unsigned old = r->notify_state[i];
    if (previous)
        *previous = r->notify[i];
    if (action == eSetValueWithoutOverwrite && old == 2) {
        arch_irq_restore(irq);
        return pdFAIL;
    }
    switch (action) {
    case eSetBits:
        r->notify[i] |= value;
        break;
    case eIncrement:
        r->notify[i]++;
        break;
    case eSetValueWithOverwrite:
    case eSetValueWithoutOverwrite:
        r->notify[i] = value;
        break;
    case eNoAction:
        break;
    default:
        configASSERT(0);
    }
    r->notify_state[i] = 2;
    if (old == 1 && !r->suspended) {
        r->native->state = TASK_READY;
        if (!self() || r->native->priority < current_task->priority) {
            if (higher)
                *higher = pdTRUE;
            pended_yield = 1;
        }
    }
    arch_irq_restore(irq);
    return pdPASS;
}
BaseType_t xTaskGenericNotify(TaskHandle_t h, UBaseType_t i, uint32_t value, eNotifyAction action,
                              uint32_t *previous)
{
    BaseType_t higher = pdFALSE;
    BaseType_t result = xTaskGenericNotifyFromISR(h, i, value, action, previous, &higher);
    if (higher && !suspended)
        vPortYield();
    return result;
}
void vTaskGenericNotifyGiveFromISR(TaskHandle_t h, UBaseType_t i, BaseType_t *higher)
{
    xTaskGenericNotifyFromISR(h, i, 0, eIncrement, NULL, higher);
}
BaseType_t xTaskGenericNotifyWait(UBaseType_t i, uint32_t entry, uint32_t exit, uint32_t *value,
                                  TickType_t ticks)
{
    struct nos_idf_task *r = self();
    configASSERT(r && i < configTASK_NOTIFICATION_ARRAY_ENTRIES);
    uintptr_t irq = arch_irq_save();
    if (r->notify_state[i] != 2) {
        r->notify[i] &= ~entry;
        if (ticks) {
            configASSERT(!suspended);
            r->notify_state[i] = 1;
            r->native->wake_tick = ticks == portMAX_DELAY ? UINT64_MAX : kernel_ticks + ticks;
            r->native->state = TASK_SLEEP;
            arch_irq_restore(irq);
            vPortYield();
            irq = arch_irq_save();
        }
    }
    BaseType_t result = r->notify_state[i] == 2 ? pdTRUE : pdFALSE;
    if (value)
        *value = r->notify[i];
    if (result)
        r->notify[i] &= ~exit;
    r->notify_state[i] = 0;
    arch_irq_restore(irq);
    return result;
}
uint32_t ulTaskGenericNotifyTake(UBaseType_t i, BaseType_t clear, TickType_t ticks)
{
    struct nos_idf_task *r = self();
    configASSERT(r && i < configTASK_NOTIFICATION_ARRAY_ENTRIES);
    uintptr_t irq = arch_irq_save();
    if (!r->notify[i] && ticks) {
        configASSERT(!suspended);
        r->notify_state[i] = 1;
        r->native->wake_tick = ticks == portMAX_DELAY ? UINT64_MAX : kernel_ticks + ticks;
        r->native->state = TASK_SLEEP;
        arch_irq_restore(irq);
        vPortYield();
        irq = arch_irq_save();
    }
    uint32_t value = r->notify[i];
    if (value)
        r->notify[i] = clear ? 0 : value - 1;
    r->notify_state[i] = 0;
    arch_irq_restore(irq);
    return value;
}
BaseType_t xTaskGenericNotifyStateClear(TaskHandle_t h, UBaseType_t i)
{
    uintptr_t irq = arch_irq_save();
    struct nos_idf_task *r = lookup(h);
    configASSERT(r && i < configTASK_NOTIFICATION_ARRAY_ENTRIES);
    BaseType_t result = r->notify_state[i] == 2;
    if (result)
        r->notify_state[i] = 0;
    arch_irq_restore(irq);
    return result;
}
uint32_t ulTaskGenericNotifyValueClear(TaskHandle_t h, UBaseType_t i, uint32_t bits)
{
    uintptr_t irq = arch_irq_save();
    struct nos_idf_task *r = lookup(h);
    configASSERT(r && i < configTASK_NOTIFICATION_ARRAY_ENTRIES);
    uint32_t result = r->notify[i];
    r->notify[i] &= ~bits;
    arch_irq_restore(irq);
    return result;
}

static void reclaim(struct nos_idf_task *r)
{
    for (unsigned j = 0; j < CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS; j++)
        if (r->tls_delete[j])
            r->tls_delete[j](j, r->tls[j]);
    _reclaim_reent(&r->reent);
#if XCHAL_CP_NUM > 0
    extern void _xt_coproc_release(volatile void *, BaseType_t);
    uintptr_t irq = arch_irq_save();
    _xt_coproc_release((void *)(((uintptr_t)r->stack_end - XT_CP_SIZE) & ~15u), 0);
    arch_irq_restore(irq);
#endif
    if (!r->static_storage)
        heap_caps_free(r->stack);
    uintptr_t saved = arch_irq_save();
    memset(r, 0, sizeof(*r));
    arch_irq_restore(saved);
}
void nos_idf_idle(void)
{
    /* Reclaim self-deleted tasks only after leaving their stacks. A record is
     * reserved until its callbacks, Newlib and coprocessor ownership are freed. */
    for (unsigned i = 0; i < NOS_IDLE_FIRST; i++) {
        struct nos_idf_task *r = &records[i];
        uintptr_t irq = arch_irq_save();
        if (r->native && r != self() && r->native->state == TASK_UNUSED && !r->deleting)
            r->deleting = 1; /* A native EXIT also follows the SDK cleanup path. */
        unsigned cleanup = r->deleting == 1 && r != self();
        if (cleanup)
            r->deleting = 2;
        arch_irq_restore(irq);
        if (cleanup)
            reclaim(r);
    }
    extern void esp_vApplicationIdleHook(void);
    esp_vApplicationIdleHook();
}

uint8_t *pxTaskGetStackStart(TaskHandle_t h)
{
    struct nos_idf_task *r = lookup(h);
    return r ? (uint8_t *)r->stack : NULL;
}
configSTACK_DEPTH_TYPE uxTaskGetStackHighWaterMark2(TaskHandle_t h)
{
    struct nos_idf_task *r = lookup(h);
    if (!r || !r->stack)
        return 0;
    const uint8_t *p = (const uint8_t *)r->stack;
    uint32_t count = 0;
    while (count < r->stack_bytes && p[count] == 0xa5)
        count++;
    return count; /* ESP-IDF stack sizes and high-water marks are bytes. */
}
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t h)
{
    return uxTaskGetStackHighWaterMark2(h);
}
eTaskState eTaskGetState(TaskHandle_t h)
{
    uintptr_t irq = arch_irq_save();
    struct nos_idf_task *r = lookup(h);
    eTaskState state = !r                               ? eDeleted
                       : r == self()                    ? eRunning
                       : r->suspended                   ? eSuspended
                       : r->native->state == TASK_READY ? eReady
                                                        : eBlocked;
    arch_irq_restore(irq);
    return state;
}
BaseType_t xTaskAbortDelay(TaskHandle_t h)
{
    uintptr_t irq = arch_irq_save();
    struct nos_idf_task *r = lookup(h);
    BaseType_t result = pdFALSE;
    if (r && !r->suspended && r->native->state == TASK_SLEEP) {
        if (listLIST_ITEM_CONTAINER(&r->event))
            uxListRemove(&r->event);
        r->waiting = 0;
        r->delay_aborted = 1;
        r->native->state = TASK_READY;
        result = pdTRUE;
    }
    arch_irq_restore(irq);
    if (result && !suspended)
        vPortYield();
    return result;
}
TaskHandle_t xTaskGetHandle(const char *name)
{
    if (!name)
        return NULL;
    uintptr_t irq = arch_irq_save();
    TaskHandle_t h = NULL;
    for (unsigned i = 0; i < NOS_TASKS; i++)
        if (records[i].native && !records[i].deleting && !strcmp(name, records[i].name)) {
            h = handle(&records[i]);
            break;
        }
    arch_irq_restore(irq);
    return h;
}
BaseType_t xTaskGetStaticBuffers(TaskHandle_t h, StackType_t **stack, StaticTask_t **buffer)
{
    struct nos_idf_task *r = lookup(h);
    if (!r || !r->static_storage || !stack || !buffer)
        return pdFALSE;
    *stack = r->stack;
    *buffer = r->static_buffer;
    return pdTRUE;
}
void vTaskGetInfo(TaskHandle_t h, TaskStatus_t *status, BaseType_t watermark, eTaskState state)
{
    struct nos_idf_task *r = lookup(h);
    configASSERT(r && status);
    memset(status, 0, sizeof(*status));
    status->xHandle = handle(r);
    status->pcTaskName = r->name;
    status->xTaskNumber = r->native->id;
    status->eCurrentState = state == eInvalid ? eTaskGetState(h) : state;
    status->uxCurrentPriority = 31 - r->native->priority;
    status->uxBasePriority = 31 - r->native->base_priority;
    status->pxStackBase = r->stack;
    if (watermark)
        status->usStackHighWaterMark = uxTaskGetStackHighWaterMark2(h);
}
UBaseType_t uxTaskGetSystemState(TaskStatus_t *array, const UBaseType_t count,
                                 configRUN_TIME_COUNTER_TYPE *runtime)
{
    uintptr_t irq = arch_irq_save();
    UBaseType_t n = uxTaskGetNumberOfTasks();
    if (!array || count < n) {
        arch_irq_restore(irq);
        return 0;
    }
    n = 0;
    for (unsigned i = 0; i < NOS_TASKS; i++)
        if (records[i].native && !records[i].deleting)
            vTaskGetInfo(handle(&records[i]), &array[n++], pdTRUE, eInvalid);
    if (runtime)
        *runtime = 0; /* Runtime accounting is disabled in this profile. */
    arch_irq_restore(irq);
    return n;
}
#include "esp_private/freertos_idf_additions_priv.h"
void prvTaskPriorityRaise(prvTaskSavedPriority_t *saved, UBaseType_t p)
{
    configASSERT(saved && p < configMAX_PRIORITIES);
    uintptr_t irq = arch_irq_save();
    saved->uxPriority = 31 - self()->ceiling_priority;
    saved->uxBasePriority = 31 - current_task->base_priority;
    if (priority(p) < self()->ceiling_priority)
        self()->ceiling_priority = priority(p);
    recompute_priority(self());
    arch_irq_restore(irq);
}
void prvTaskPriorityRestore(prvTaskSavedPriority_t *saved)
{
    configASSERT(saved);
    uintptr_t irq = arch_irq_save();
    self()->ceiling_priority = priority(saved->uxPriority);
    recompute_priority(self());
    arch_irq_restore(irq);
    if (!suspended)
        vPortYield();
}

#include "esp_private/freertos_debug.h"
void *pvTaskGetCurrentTCBForCore(BaseType_t core)
{
    return core == 0 ? self() : NULL;
}
int xTaskGetNext(TaskIterator_t *iterator)
{
    if (!iterator)
        return -1;
    for (unsigned i = iterator->uxCurrentListIndex; i < NOS_TASKS; i++) {
        struct nos_idf_task *r = &records[i];
        if (r->native && !r->deleting) {
            iterator->uxCurrentListIndex = i + 1;
            iterator->pxTaskHandle = handle(r);
            return (int)i;
        }
    }
    iterator->pxTaskHandle = NULL;
    return -1;
}
BaseType_t vTaskGetSnapshot(TaskHandle_t h, TaskSnapshot_t *snapshot)
{
    struct nos_idf_task *r = lookup(h);
    if (!r || !snapshot)
        return pdFALSE;
    *snapshot = (TaskSnapshot_t){.pxTCB = r, .pxTopOfStack = r->top, .pxEndOfStack = r->stack_end};
    return pdTRUE;
}
UBaseType_t uxTaskGetSnapshotAll(TaskSnapshot_t *array, const UBaseType_t count, UBaseType_t *size)
{
    if (size)
        *size = sizeof(struct nos_idf_task);
    if (!array)
        return 0;
    UBaseType_t n = 0;
    for (unsigned i = 0; i < NOS_TASKS && n < count; i++)
        if (records[i].native && !records[i].deleting)
            vTaskGetSnapshot(handle(&records[i]), &array[n++]);
    return n;
}
