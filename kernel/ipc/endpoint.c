/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS Microkernel Synchronous Rendezvous Endpoint IPC
 * Supports fixed 64-byte struct nos_ipc_msg, unforgeable client badge,
 * capability handle delegation, and priority inheritance.
 */

#define pr_fmt(fmt) "[IPC_EP]:%s[%d]:"fmt, __func__, __LINE__

#include <nos/ipc.h>
#include <nos/cap.h>
#include <uapi/nos/errno.h>
#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <kernel/task.h>
#include <kernel/sched.h>
#include <kernel/timer.h>
#include <kernel/printk.h>
#include <string.h>

static uint32_t g_next_endpoint_id = 1U;

static void ipc_queue_insert_prio(struct list_head *queue,
                                  spinlock_t *queue_lock,
                                  struct task_struct *task)
{
    struct task_struct *cur;

    list_for_each_entry(cur, queue, list) {
        if (task->current_priority < cur->current_priority) {
            break;
        }
    }
    list_add_tail(&task->list, &cur->list);
    task->list_lock = queue_lock;
}

static void ipc_transfer_caps(struct task_struct *src_task,
                              struct task_struct *dst_task,
                              struct nos_ipc_msg *msg)
{
    uint32_t flags;
    uint32_t cap_cnt;
    uint32_t i;
    uint32_t granted = 0U;

    if (src_task == NULL || dst_task == NULL || msg == NULL) {
        return;
    }

    flags = NOS_IPC_TAG_FLAGS(msg->tag);
    cap_cnt = NOS_IPC_TAG_CAP_CNT(msg->tag);
    if (!(flags & NOS_IPC_FLAG_CAP_XFER) || cap_cnt == 0U) {
        return;
    }
    if (cap_cnt > NOS_IPC_MAX_CAPS) {
        cap_cnt = NOS_IPC_MAX_CAPS;
    }

    for (i = 0U; i < cap_cnt; i++) {
        nos_handle_t dst_h = NOS_HANDLE_INVALID;
        if (msg->caps[i] != NOS_HANDLE_INVALID) {
            int rc = nos_cap_derive(&src_task->cap_table,
                                    msg->caps[i],
                                    &dst_task->cap_table,
                                    0U,
                                    0U,
                                    &dst_h);
            if (rc == 0) {
                granted++;
            }
        }
        msg->caps[i] = dst_h;
    }

    msg->tag = NOS_IPC_TAG_MAKE(NOS_IPC_TAG_OPCODE(msg->tag), flags, granted);
}

int nos_endpoint_init(struct nos_endpoint *ep, uint32_t id)
{
    if (ep == NULL) {
        return -EINVAL;
    }

    spin_lock_init(&ep->lock);
    INIT_LIST_HEAD(&ep->send_queue);
    INIT_LIST_HEAD(&ep->recv_queue);
    ep->id = (id != 0U) ? id : g_next_endpoint_id++;
    ep->closed = false;

    return 0;
}

struct nos_endpoint *nos_endpoint_create(nos_handle_t *out_cap)
{
    struct nos_endpoint *ep;
    struct task_struct *task = current;
    int rc;

    ep = kmalloc(sizeof(struct nos_endpoint), GFP_KERNEL);
    if (ep == NULL) {
        return NULL;
    }
    nos_endpoint_init(ep, 0U);

    if (out_cap != NULL && task != NULL) {
        rc = nos_cap_alloc(&task->cap_table,
                           NOS_CAP_ENDPOINT,
                           ep,
                           NOS_CAP_RIGHT_ALL,
                           0U,
                           out_cap);
        if (rc < 0) {
            kfree(ep);
            return NULL;
        }
    }

    return ep;
}

void nos_endpoint_close(struct nos_endpoint *ep)
{
    struct task_struct *task, *tmp;
    LIST_HEAD(wake_list);

    if (ep == NULL) {
        return;
    }

    spin_lock_irq(&ep->lock);
    ep->closed = true;
    list_for_each_entry_safe(task, tmp, &ep->send_queue, list) {
        list_del(&task->list);
        task->list_lock = NULL;
        task->flag = -EPIPE;
        task->ipc_state = NOS_IPC_STATE_NONE;
        list_add_tail(&task->list, &wake_list);
    }
    list_for_each_entry_safe(task, tmp, &ep->recv_queue, list) {
        list_del(&task->list);
        task->list_lock = NULL;
        task->flag = -EPIPE;
        task->ipc_state = NOS_IPC_STATE_NONE;
        list_add_tail(&task->list, &wake_list);
    }
    spin_unlock_irq(&ep->lock);

    list_for_each_entry_safe(task, tmp, &wake_list, list) {
        list_del_init(&task->list);
        task_resume(task);
    }
    switch_task();
}

void nos_endpoint_destroy(struct nos_endpoint *ep)
{
    if (ep == NULL) {
        return;
    }
    nos_endpoint_close(ep);
    kfree(ep);
}

int nos_endpoint_call(struct nos_endpoint *ep,
                      uint32_t badge,
                      const struct nos_ipc_msg *req,
                      struct nos_ipc_msg *resp,
                      uint32_t timeout_ticks)
{
    struct task_struct *client = current;
    struct task_struct *server = NULL;
    int rc;

    if (ep == NULL || req == NULL || client == NULL) {
        return -EINVAL;
    }

    spin_lock_irq(&ep->lock);
    if (ep->closed) {
        spin_unlock_irq(&ep->lock);
        return -EPIPE;
    }

    client->ipc_msg = *req;
    client->ipc_msg.token = (badge != 0U) ? badge : (uint32_t)client->pid;
    client->flag = 0;

    if (!list_empty(&ep->recv_queue)) {
        /* Fastpath: a server thread is already waiting to receive */
        server = list_first_entry(&ep->recv_queue, struct task_struct, list);
        list_del_init(&server->list);
        server->list_lock = NULL;
        server->ipc_state = NOS_IPC_STATE_NONE;

        ipc_transfer_caps(client, server, &client->ipc_msg);
        server->ipc_msg = client->ipc_msg;
        server->ipc_partner = client;
        client->ipc_partner = server;
        client->ipc_state = NOS_IPC_STATE_REPLY_WAIT;
        spin_unlock_irq(&ep->lock);

        /* Priority inheritance: boost server if caller has higher priority */
        if (client->current_priority < server->current_priority) {
            (void)task_set_prio(server, client->current_priority);
        }

        spin_lock_irq(&client->lock);
        rc = task_hang_lock(client);
        if (rc < 0) {
            spin_unlock_irq(&client->lock);
            return rc;
        }
        if (timeout_ticks != NOS_IPC_TIMEOUT_INFINITE &&
            timeout_ticks != NOS_IPC_TIMEOUT_NONBLOCK) {
            timer_start(&client->timer, timeout_ticks);
        }
        spin_unlock_irq(&client->lock);

        task_resume(server);
        switch_task();
    } else {
        /* Slowpath: no server waiting yet */
        if (timeout_ticks == NOS_IPC_TIMEOUT_NONBLOCK ||
            (NOS_IPC_TAG_FLAGS(req->tag) & NOS_IPC_FLAG_NONBLOCK)) {
            spin_unlock_irq(&ep->lock);
            return -EAGAIN;
        }

        spin_lock_irq(&client->lock);
        rc = task_hang_lock(client);
        if (rc < 0) {
            spin_unlock_irq(&client->lock);
            spin_unlock_irq(&ep->lock);
            return rc;
        }
        client->ipc_state = NOS_IPC_STATE_SEND_WAIT;
        client->ipc_partner = NULL;
        ipc_queue_insert_prio(&ep->send_queue, &ep->lock, client);
        if (timeout_ticks != NOS_IPC_TIMEOUT_INFINITE) {
            timer_start(&client->timer, timeout_ticks);
        }
        spin_unlock_irq(&client->lock);
        spin_unlock_irq(&ep->lock);

        switch_task();
    }

    if (client->flag < 0) {
        rc = client->flag;
        client->flag = 0;
        client->ipc_state = NOS_IPC_STATE_NONE;
        client->ipc_partner = NULL;
        return rc;
    }

    if (resp != NULL) {
        *resp = client->ipc_msg;
    }
    return client->ipc_msg.status;
}

int nos_endpoint_recv(struct nos_endpoint *ep,
                      struct nos_ipc_msg *msg,
                      struct task_struct **out_caller,
                      uint32_t timeout_ticks)
{
    struct task_struct *server = current;
    struct task_struct *client = NULL;
    int rc;

    if (ep == NULL || msg == NULL || server == NULL) {
        return -EINVAL;
    }

    spin_lock_irq(&ep->lock);
    if (ep->closed) {
        spin_unlock_irq(&ep->lock);
        return -EPIPE;
    }

    if (!list_empty(&ep->send_queue)) {
        /* Fastpath: dequeue highest-priority waiting sender */
        client = list_first_entry(&ep->send_queue, struct task_struct, list);
        list_del_init(&client->list);
        client->list_lock = NULL;
        client->ipc_state = NOS_IPC_STATE_REPLY_WAIT;
        client->ipc_partner = server;
        server->ipc_partner = client;

        ipc_transfer_caps(client, server, &client->ipc_msg);
        *msg = client->ipc_msg;
        spin_unlock_irq(&ep->lock);

        if (client->current_priority < server->current_priority) {
            (void)task_set_prio(server, client->current_priority);
        }
        if (out_caller != NULL) {
            *out_caller = client;
        }
        return 0;
    }

    if (timeout_ticks == NOS_IPC_TIMEOUT_NONBLOCK) {
        spin_unlock_irq(&ep->lock);
        return -EAGAIN;
    }

    spin_lock_irq(&server->lock);
    rc = task_hang_lock(server);
    if (rc < 0) {
        spin_unlock_irq(&server->lock);
        spin_unlock_irq(&ep->lock);
        return rc;
    }
    server->flag = 0;
    server->ipc_state = NOS_IPC_STATE_RECV_WAIT;
    server->ipc_partner = NULL;
    ipc_queue_insert_prio(&ep->recv_queue, &ep->lock, server);
    if (timeout_ticks != NOS_IPC_TIMEOUT_INFINITE) {
        timer_start(&server->timer, timeout_ticks);
    }
    spin_unlock_irq(&server->lock);
    spin_unlock_irq(&ep->lock);

    switch_task();

    if (server->flag < 0) {
        rc = server->flag;
        server->flag = 0;
        server->ipc_state = NOS_IPC_STATE_NONE;
        server->ipc_partner = NULL;
        return rc;
    }

    *msg = server->ipc_msg;
    if (out_caller != NULL) {
        *out_caller = server->ipc_partner;
    }
    return 0;
}

int nos_endpoint_reply_wait(struct nos_endpoint *ep,
                            struct task_struct *caller,
                            const struct nos_ipc_msg *reply,
                            struct nos_ipc_msg *next_req,
                            struct task_struct **out_next_caller,
                            uint32_t timeout_ticks)
{
    struct task_struct *server = current;
    struct task_struct *target;

    if (server == NULL) {
        return -EINVAL;
    }

    target = (caller != NULL) ? caller : server->ipc_partner;
    if (reply != NULL && target != NULL) {
        if (target->ipc_state == NOS_IPC_STATE_REPLY_WAIT &&
            target->ipc_partner == server) {
            target->ipc_msg = *reply;
            ipc_transfer_caps(server, target, &target->ipc_msg);
            target->ipc_state = NOS_IPC_STATE_NONE;
            target->ipc_partner = NULL;
            server->ipc_partner = NULL;

            /* Restore server priority after completing priority-inherited RPC */
            if (server->current_priority != server->init_priority) {
                (void)task_set_prio(server, server->init_priority);
            }

            task_resume(target);
        }
    }

    if (ep != NULL && next_req != NULL) {
        return nos_endpoint_recv(ep, next_req, out_next_caller, timeout_ticks);
    }

    switch_task();
    return 0;
}

int nos_ipc_call(nos_handle_t ep_cap,
                 const struct nos_ipc_msg *req,
                 struct nos_ipc_msg *resp,
                 uint32_t timeout_ticks)
{
    struct task_struct *task = current;
    struct nos_endpoint *ep = NULL;
    uint32_t badge = 0U;
    int rc;

    if (task == NULL) {
        return -EINVAL;
    }

    rc = nos_cap_lookup(&task->cap_table,
                        ep_cap,
                        NOS_CAP_ENDPOINT,
                        NOS_CAP_RIGHT_SEND,
                        (void **)&ep,
                        &badge);
    if (rc < 0) {
        return rc;
    }

    return nos_endpoint_call(ep, badge, req, resp, timeout_ticks);
}

int nos_ipc_recv(nos_handle_t ep_cap,
                 struct nos_ipc_msg *msg,
                 uint32_t timeout_ticks)
{
    struct task_struct *task = current;
    struct nos_endpoint *ep = NULL;
    int rc;

    if (task == NULL) {
        return -EINVAL;
    }

    rc = nos_cap_lookup(&task->cap_table,
                        ep_cap,
                        NOS_CAP_ENDPOINT,
                        NOS_CAP_RIGHT_RECV,
                        (void **)&ep,
                        NULL);
    if (rc < 0) {
        return rc;
    }

    return nos_endpoint_recv(ep, msg, NULL, timeout_ticks);
}

int nos_ipc_reply_wait(nos_handle_t ep_cap,
                       const struct nos_ipc_msg *reply,
                       struct nos_ipc_msg *next_req,
                       uint32_t timeout_ticks)
{
    struct task_struct *task = current;
    struct nos_endpoint *ep = NULL;
    int rc;

    if (task == NULL) {
        return -EINVAL;
    }

    if (ep_cap != NOS_HANDLE_INVALID && next_req != NULL) {
        rc = nos_cap_lookup(&task->cap_table,
                            ep_cap,
                            NOS_CAP_ENDPOINT,
                            NOS_CAP_RIGHT_RECV,
                            (void **)&ep,
                            NULL);
        if (rc < 0) {
            return rc;
        }
    }

    return nos_endpoint_reply_wait(ep,
                                   task->ipc_partner,
                                   reply,
                                   next_req,
                                   NULL,
                                   timeout_ticks);
}
