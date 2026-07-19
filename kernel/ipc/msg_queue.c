/*
 * Copyright (C) 2018 胡启航<Hu Qihang>
 *
 * Author: wkcs
 *
 * Email: hqh2030@gmail.com, huqihan@live.com
 */

#define pr_fmt(fmt) "[MSG_Q]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/msg_queue.h>
#include <kernel/mm.h>
#include <kernel/printk.h>
#include <string.h>

static LIST_HEAD(g_msg_q_list);
static SPINLOCK(g_msg_q_lock);

int msg_q_init(struct msg_queue *msg_q)
{
    if (msg_q == NULL) {
        pr_err("msg_q is NULL\r\n");
        return -EINVAL;
    }

    spin_lock_init(&msg_q->lock);
    INIT_LIST_HEAD(&msg_q->msg_list);
    INIT_LIST_HEAD(&msg_q->list);
    msg_q->owner = NULL;

    spin_lock_irq(&g_msg_q_lock);
    list_add_tail(&msg_q->list, &g_msg_q_list);
    spin_unlock_irq(&g_msg_q_lock);

    return 0;
}

int msg_q_remove(struct msg_queue *msg_q)
{
    /* TODO */
    return 0;
}

static msg_t *msg_alloc(int size, gfp_t flag)
{
    msg_t *msg;

    msg = kmalloc(size + sizeof(msg_t), flag);
    if (msg == NULL) {
        return NULL;
    }
    INIT_LIST_HEAD(&msg->list);
    msg->size = size;

    return msg;
}

static int msg_free(msg_t *msg)
{
    return kfree(msg);
}

int msg_q_send(struct msg_queue *msg_q, const char *buf, int size)
{
    msg_t *msg;

    if (msg_q == NULL) {
        pr_err("msg_q is NULL\r\n");
        return -EINVAL;
    }
    if (buf == NULL) {
        pr_err("buf is NULL\r\n");
        return -EINVAL;
    }

    msg = msg_alloc(size, GFP_KERNEL);
    if (msg == NULL) {
        pr_err("alloc msg buf error\r\n");
        return -EINVAL;
    }
    memcpy(msg->buf, buf, size);

    spin_lock_irq(&msg_q->lock);
    list_add_tail(&msg->list, &msg_q->msg_list);
    spin_unlock_irq(&msg_q->lock);

    if (msg_q->owner != NULL && msg_q->owner->status == TASK_WAIT) {
        task_resume(msg_q->owner);
        switch_task();
    }

    return 0;
}

static msg_t *__msg_q_recv(struct msg_queue *msg_q)
{
    msg_t *msg;

    spin_lock_irq(&msg_q->lock);
    if (list_empty(&msg_q->msg_list)) {
        spin_unlock_irq(&msg_q->lock);
        pr_err("msg list is empty\r\n");
        return NULL;
    }

    msg = list_first_entry(&msg_q->msg_list, msg_t, list);
    list_del(&msg->list);
    spin_unlock_irq(&msg_q->lock);

    return msg;
}

int msg_q_recv(struct msg_queue *msg_q, char *buf, int size)
{
    struct task_struct *task;
    msg_t *msg;
    int recv_size;
    int rc;

    if (msg_q == NULL) {
        pr_err("msg_q is NULL\r\n");
        return -EINVAL;
    }
    if (buf == NULL) {
        pr_err("buf is NULL\r\n");
        return -EINVAL;
    }

    task = current;
    if (msg_q->owner == NULL) {
        msg_q->owner = task;
    } else if (msg_q->owner != task) {
        pr_err("msg_q owner is %s, %s can't use recv it's msg\r\n", msg_q->owner->name, task->name);
        return -EFAULT;
    }
    spin_lock_irq(&msg_q->lock);
    if (list_empty(&msg_q->msg_list)) {
        spin_unlock_irq(&msg_q->lock);
        rc = task_hang(task);
        if (rc < 0) {
            pr_err("%s task hang error, rc=%d\r\n", task->name, rc);
            return rc;
        }
        switch_task();
    } else {
        spin_unlock_irq(&msg_q->lock);
    }

    msg = __msg_q_recv(msg_q);
    if (msg == NULL) {
        pr_err("recv msg error\r\n");
        return -EINVAL;
    }

    if (msg->size > size) {
        pr_err("buf size(=%u) too small, msg size is %u\r\n", size, msg->size);
        recv_size = size;
    } else {
        recv_size = msg->size;
    }
    memcpy(buf, msg->buf, recv_size);
    msg_free(msg);

    return recv_size;
}

int msg_q_recv_timeout(struct msg_queue *msg_q, char *buf, int size, u32 tick)
{
    struct task_struct *task;
    msg_t *msg;
    int recv_size;
    int rc;

    if (msg_q == NULL) {
        pr_err("msg_q is NULL\r\n");
        return -EINVAL;
    }
    if (buf == NULL) {
        pr_err("buf is NULL\r\n");
        return -EINVAL;
    }

    task = current;
    if (msg_q->owner == NULL) {
        msg_q->owner = task;
    } else if (msg_q->owner != task) {
        pr_err("msg_q owner is %s, %s can't use recv it's msg\r\n", msg_q->owner->name, task->name);
        return -EFAULT;
    }

    spin_lock_irq(&msg_q->lock);
    if (list_empty(&msg_q->msg_list)) {
        spin_unlock_irq(&msg_q->lock);
        spin_lock_irq(&task->lock);
        rc = task_hang_lock(task);
        if (rc < 0) {
            spin_unlock_irq(&task->lock);
            pr_err("%s task hang error, rc=%d\r\n", task->name, rc);
            return rc;
        }
        timer_start(&task->timer, tick);
        spin_unlock_irq(&task->lock);
        switch_task();
    } else {
        spin_unlock_irq(&msg_q->lock);
    }

    if (task->flag == -ETIMEDOUT) {
        task->flag = 0;
        return -ETIMEDOUT;
    }
    msg = __msg_q_recv(msg_q);
    if (msg == NULL) {
        pr_err("recv msg error\r\n");
        return -EINVAL;
    }

    if (msg->size > size) {
        pr_err("buf size(=%u) too small, msg size is %u\r\n", size, msg->size);
        recv_size = size;
    } else {
        recv_size = msg->size;
    }
    memcpy(buf, msg->buf, recv_size);
    msg_free(msg);

    return recv_size;
}
