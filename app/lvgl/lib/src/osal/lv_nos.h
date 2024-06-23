/**
 * @file lv_nos.h
 *
 */

#ifndef LV_NOS_H
#define LV_NOS_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#if LV_USE_OS == LV_OS_NOS

#include <kernel/kernel.h>
#include <kernel/task.h>
#include <kernel/mutex.h>
#include <kernel/sem.h>

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/
typedef struct {
    struct task_struct *task;
} lv_thread_t;

typedef struct {
    struct mutex mutex;
} lv_mutex_t;

typedef struct {
    sem_t sem;
} lv_thread_sync_t;

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**********************
 *      MACROS
 **********************/

#endif /*LV_USE_OS == LV_OS_RTTHREAD*/

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LV_NOS_H*/
