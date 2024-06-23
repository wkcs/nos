/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_SEM_H__
#define __NOS_SEM_H__

#include <kernel/kernel.h>
#include <kernel/list.h>
#include <kernel/spinlock.h>

typedef struct {
    uint8_t count;
    struct list_head wait_list;
    spinlock_t lock;
} sem_t;

int sem_init(sem_t *sem, uint8_t num);
void sem_get(sem_t *sem);
int sem_get_timeout(sem_t *sem, uint32_t tick);
void sem_send(sem_t *sem, uint8_t num);
void sem_send_all(sem_t *sem);

#define sem_send_one(sem) sem_send(sem, 1)

#endif /* __NOS_SEM_H__ */
