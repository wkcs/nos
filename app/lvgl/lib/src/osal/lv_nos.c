/**
 * @file lv_nos.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "lv_os.h"

#if LV_USE_OS == LV_OS_NOS

#include "../misc/lv_log.h"

/*********************
 *      DEFINES
 *********************/

#define THREAD_TIMESLICE 10U

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_result_t lv_thread_init(lv_thread_t * thread, lv_thread_prio_t prio, void (*callback)(void *), size_t stack_size,
                           void * user_data)
{
    thread->task = task_create("lvgl",
                               callback,
                               user_data,
                               prio,
                               stack_size,
                               THREAD_TIMESLICE,
                               NULL);
    int ret = task_ready(thread->task);
    if(ret) {
        LV_LOG_WARN("Error: %d", ret);
        return LV_RESULT_INVALID;
    }
    else {
        return LV_RESULT_OK;
    }
}

lv_result_t lv_thread_delete(lv_thread_t * thread)
{
    task_del(thread->task);
    return LV_RESULT_OK;
}

lv_result_t lv_mutex_init(lv_mutex_t * mutex)
{
    mutex_init(&mutex->mutex);
    return LV_RESULT_OK;
}

lv_result_t lv_mutex_lock(lv_mutex_t * mutex)
{
    mutex_lock(&mutex->mutex);
    return LV_RESULT_OK;
}

lv_result_t lv_mutex_lock_isr(lv_mutex_t * mutex)
{
    mutex_lock(&mutex->mutex);
    return LV_RESULT_OK;
}

lv_result_t lv_mutex_unlock(lv_mutex_t * mutex)
{
    mutex_unlock(&mutex->mutex);
    return LV_RESULT_OK;
}

lv_result_t lv_mutex_delete(lv_mutex_t * mutex)
{
    return LV_RESULT_OK;
}

lv_result_t lv_thread_sync_init(lv_thread_sync_t * sync)
{
    int rc = sem_init(&sync->sem, 0);
    if(rc < 0) {
        LV_LOG_WARN("create semaphore failed");
        return LV_RESULT_INVALID;
    } else {
        return LV_RESULT_OK;
    }
}

lv_result_t lv_thread_sync_wait(lv_thread_sync_t * sync)
{
    sem_get(&sync->sem);
    return LV_RESULT_OK;
}

lv_result_t lv_thread_sync_signal(lv_thread_sync_t * sync)
{
    sem_send_all(&sync->sem);
    return LV_RESULT_OK;
}

lv_result_t lv_thread_sync_delete(lv_thread_sync_t * sync)
{
    return LV_RESULT_OK;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

#endif /*LV_USE_OS == LV_OS_NOS*/
