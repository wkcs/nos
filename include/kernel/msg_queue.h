/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_MSG_QUEUE_H__
#define __NOS_MSG_QUEUE_H__

#include <kernel/kernel.h>
#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <kernel/task.h>

typedef struct {
    struct list_head list;
    int size;
    char buf[0];
} msg_t;

struct msg_queue {
    struct task_struct *owner;
    struct list_head msg_list;
    struct list_head list;
    spinlock_t lock;
};

int msg_q_init(struct msg_queue *msg_q);
int msg_q_remove(struct msg_queue *msg_q);
int msg_q_send(struct msg_queue *msg_q, const char *buf, int size);
int msg_q_recv(struct msg_queue *msg_q, char *buf, int size);
int msg_q_recv_timeout(struct msg_queue *msg_q, char *buf, int size, u32 tick);

#endif /* __NOS_MSG_QUEUE_H__ */
