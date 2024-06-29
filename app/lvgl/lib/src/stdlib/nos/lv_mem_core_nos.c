/**
 * @file lv_malloc_core_nos.c
 */

/*********************
 *      INCLUDES
 *********************/
#include "../lv_mem.h"
#if LV_USE_STDLIB_MALLOC == LV_STDLIB_NOS
#include "../../stdlib/lv_mem.h"
#include <kernel/kernel.h>
#include <kernel/mm.h>

/*********************
 *      DEFINES
 *********************/

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

void lv_mem_init(void)
{
    return; /*Nothing to init*/
}

void lv_mem_deinit(void)
{
    return; /*Nothing to deinit*/
}

lv_mem_pool_t lv_mem_add_pool(void * mem, size_t bytes)
{
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    return;
}

void * lv_malloc_core(size_t size)
{
    return kmalloc(size, GFP_KERNEL);
}

void * lv_realloc_core(void * p, size_t new_size)
{
    return krealloc(p, new_size, GFP_KERNEL);
}

void lv_free_core(void * p)
{
    kfree(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t * mon_p)
{
    return;
}

lv_result_t lv_mem_test_core(void)
{
    /*Not supported*/
    return LV_RESULT_OK;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

#endif /*LV_STDLIB_NOS*/
