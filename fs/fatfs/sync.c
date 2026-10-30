/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <kernel/sem.h>
#include <kernel/task.h>
#include <kernel/mm.h>
#include "ff.h"

struct fatfs_sync_object {
    sem_t available;
};

static struct fatfs_sync_object fatfs_sync_objects[_VOLUMES];

void *ff_memalloc(UINT size)
{
    return kmalloc(size, GFP_KERNEL);
}

void ff_memfree(void *buffer)
{
    kfree(buffer);
}

int ff_cre_syncobj(BYTE vol, _SYNC_t *object)
{
    if (object == NULL || vol >= _VOLUMES) return 0;
    *object = &fatfs_sync_objects[vol];
    return sem_init(&(*object)->available, 1) == 0;
}

int ff_del_syncobj(_SYNC_t object)
{
    return object != NULL;
}

int ff_req_grant(_SYNC_t object)
{
    struct task_struct *task = task_current();
    if (object == NULL) return 0;
    /* Initialization accesses FatFs synchronously before the scheduler starts. */
    if (task == NULL) return 1;
    if (task->pid == IDEL_TASK_PID) return 0;
    return sem_get_timeout(&object->available, _FS_TIMEOUT) == 0;
}

void ff_rel_grant(_SYNC_t object)
{
    if (object != NULL && task_current() != NULL) sem_send_one(&object->available);
}
