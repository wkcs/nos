/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS Microkernel Synchronous Rendezvous Endpoint IPC
 * Supports fixed 64-byte struct nos_ipc_msg, unforgeable client badge,
 * in-flight reply_queue tracking, capability handle delegation,
 * and priority inheritance.
 */

#define pr_fmt(fmt) "[IPC_EP]:%s[%d]:" fmt, __func__, __LINE__

#include <nos/ipc.h>
#include <nos/cap.h>
#include <nos/object.h>
#include <nos/wait.h>
#include <nos/timeout.h>
#include <uapi/nos/errno.h>
#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <kernel/task.h>
#include <kernel/sched.h>
#include <kernel/timer.h>
#include <kernel/printk.h>
#include <string.h>

static uint32_t g_next_endpoint_id = 1U;

static inline nos_timeout_t ipc_timeout_to_generic(uint32_t timeout_ticks)
{
    if (timeout_ticks == NOS_IPC_TIMEOUT_NONBLOCK) {
        return NOS_NO_WAIT;
    }
    if (timeout_ticks == NOS_IPC_TIMEOUT_INFINITE) {
        return NOS_WAIT_FOREVER;
    }
    return (nos_timeout_t)timeout_ticks;
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
    int rc;

    if (ep == NULL) {
        return -EINVAL;
    }

    rc = nos_object_init(&ep->object, true);
    if (rc != 0) {
        return rc;
    }

    nos_wait_queue_init(&ep->send_queue);
    nos_wait_queue_init(&ep->recv_queue);
    INIT_LIST_HEAD(&ep->reply_queue);
    ep->id = (id != 0U) ? id : g_next_endpoint_id++;

    return 0;
}

static void nos_endpoint_release(struct nos_object *obj)
{
    struct nos_endpoint *ep = container_of(obj, struct nos_endpoint, object);

    if (ep != NULL && !obj->is_static) {
        kfree(ep);
    }
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

    rc = nos_endpoint_init(ep, 0U);
    if (rc != 0) {
        kfree(ep);
        return NULL;
    }

    ep->object.is_static = false;
    nos_object_set_release(&ep->object, nos_endpoint_release);

    if (out_cap != NULL && task != NULL) {
        rc = nos_cap_alloc(&task->cap_table,
                           NOS_CAP_ENDPOINT,
                           ep,
                           NOS_CAP_RIGHT_ALL,
                           0U,
                           out_cap);
        if (rc < 0) {
            nos_endpoint_destroy(ep);
            return NULL;
        }
    }

    return ep;
}

void nos_endpoint_close(struct nos_endpoint *ep)
{
    struct nos_wake_batch batch;
    struct nos_wait_entry *entry, *tmp_entry;

    if (ep == NULL) {
        return;
    }

    nos_wake_batch_init(&batch);

    spin_lock_irq(&ep->object.lock);
    if (ep->object.closed) {
        spin_unlock_irq(&ep->object.lock);
        return;
    }
    ep->object.closed = true;

    /* 遍历 send_queue，全部以 -EPIPE 完成 */
    while (!nos_wait_queue_empty(&ep->send_queue)) {
        entry = nos_wait_queue_first(&ep->send_queue);
        if (entry == NULL) {
            break;
        }
        (void)nos_wait_complete_locked(entry, -EPIPE, &batch);
    }

    /* 遍历 recv_queue，全部以 -EPIPE 完成 */
    while (!nos_wait_queue_empty(&ep->recv_queue)) {
        entry = nos_wait_queue_first(&ep->recv_queue);
        if (entry == NULL) {
            break;
        }
        (void)nos_wait_complete_locked(entry, -EPIPE, &batch);
    }

    /* 遍历 reply_queue，全部以 -EPIPE 完成 (消除悬挂死锁) */
    list_for_each_entry_safe(entry, tmp_entry, &ep->reply_queue, wait_node) {
        (void)nos_wait_complete_locked(entry, -EPIPE, &batch);
    }

    spin_unlock_irq(&ep->object.lock);

    /* 在对象锁外安全分发延后唤醒批次，避免反向锁序 */
    nos_wake_batch_dispatch(&batch);
}

void nos_endpoint_destroy(struct nos_endpoint *ep)
{
    if (ep == NULL) {
        return;
    }

    nos_endpoint_close(ep);
    if (!ep->object.is_static) {
        nos_object_retire(&ep->object);
    }
}

int nos_endpoint_call(struct nos_endpoint *ep,
                      uint32_t badge,
                      const struct nos_ipc_msg *req,
                      struct nos_ipc_msg *resp,
                      uint32_t timeout_ticks)
{
    struct task_struct *client = current;
    struct task_struct *server = NULL;
    struct nos_wake_batch batch;
    nos_timeout_t timeout;
    uint64_t deadline;
    bool boosted = false;
    uint8_t boost_prio = 0;
    int rc;

    if (ep == NULL || req == NULL || client == NULL || client->pid == IDEL_TASK_PID) {
        return -EINVAL;
    }

    timeout = ipc_timeout_to_generic(timeout_ticks);
    if (NOS_IPC_TAG_FLAGS(req->tag) & NOS_IPC_FLAG_NONBLOCK) {
        timeout = NOS_NO_WAIT;
    }

    rc = nos_deadline_from_timeout(timeout, &deadline);
    if (rc < 0) {
        return rc;
    }

    rc = nos_wait_prepare(&client->wait_entry, &ep->object, deadline);
    if (rc < 0) {
        return rc;
    }

    nos_wake_batch_init(&batch);

    spin_lock_irq(&client->lock);
    spin_lock_irq(&ep->object.lock);

    if (ep->object.closed) {
        spin_unlock_irq(&ep->object.lock);
        spin_unlock_irq(&client->lock);
        nos_wait_finish(&client->wait_entry);
        return -EPIPE;
    }

    client->ipc_msg = *req;
    client->ipc_msg.token = (badge != 0U) ? badge : (uint32_t)client->pid;
    client->flag = 0;

    if (!nos_wait_queue_empty(&ep->recv_queue)) {
        /* 快路径：已有服务端线程正在 recv_queue 中等待接收请求 */
        struct nos_wait_entry *server_entry = nos_wait_queue_first(&ep->recv_queue);
        server = server_entry->task;

        /* 将请求数据与权限委托交接给服务端 */
        ipc_transfer_caps(client, server, &client->ipc_msg);
        server->ipc_msg = client->ipc_msg;
        server->ipc_partner = client;
        client->ipc_partner = server;
        client->ipc_state = NOS_IPC_STATE_REPLY_WAIT;
        server->ipc_state = NOS_IPC_STATE_NONE;

        /* 完成服务端的接收等待，加入唤醒批次 */
        (void)nos_wait_complete_locked(server_entry, 0, &batch);

        /* 客户端已在 wait_entry 中进入 WAITING，加入 ep->reply_queue 跟踪在途事务 */
        list_add_tail(&client->wait_entry.wait_node, &ep->reply_queue);
        client->wait_entry.queue = NULL;

        task_set_wchan(client, "ipc_reply", (uintptr_t)ep);
        rc = task_hang_lock(client);
        if (rc < 0) {
            list_del_init(&client->wait_entry.wait_node);
            nos_wait_finish(&client->wait_entry);
            task_clear_wchan(client);
            spin_unlock_irq(&ep->object.lock);
            spin_unlock_irq(&client->lock);
            nos_wake_batch_dispatch(&batch);
            return rc;
        }

        if (timeout != NOS_WAIT_FOREVER) {
            timer_start(&client->timer, (uint32_t)timeout);
        }

        /* 优先级继承：若客户端优先级高于服务端，标记需 boost 服务端 */
        if (client->current_priority < server->current_priority) {
            boosted = true;
            boost_prio = client->current_priority;
        }

        spin_unlock_irq(&ep->object.lock);
        spin_unlock_irq(&client->lock);

        if (boosted) {
            (void)task_pi_boost(server, boost_prio);
        }

        /* 在对象锁外分发唤醒服务端 */
        nos_wake_batch_dispatch(&batch);
        switch_task();
    } else {
        /* 慢路径：暂无服务端等待 */
        if (timeout == NOS_NO_WAIT) {
            spin_unlock_irq(&ep->object.lock);
            spin_unlock_irq(&client->lock);
            nos_wait_finish(&client->wait_entry);
            return -EAGAIN;
        }

        task_set_wchan(client, "ipc_send", (uintptr_t)ep);
        rc = task_hang_lock(client);
        if (rc < 0) {
            nos_wait_finish(&client->wait_entry);
            task_clear_wchan(client);
            spin_unlock_irq(&ep->object.lock);
            spin_unlock_irq(&client->lock);
            return rc;
        }

        client->ipc_state = NOS_IPC_STATE_SEND_WAIT;
        client->ipc_partner = NULL;
        nos_wait_queue_add_prio(&ep->send_queue, &client->wait_entry);

        if (timeout != NOS_WAIT_FOREVER) {
            timer_start(&client->timer, (uint32_t)timeout);
        }

        spin_unlock_irq(&ep->object.lock);
        spin_unlock_irq(&client->lock);

        switch_task();
    }

    /*
     * 客户端被唤醒后（可能由服务端 reply 唤醒、endpoint 关闭唤醒，或由 timer 定时器超时唤醒）：
     * 在对象锁内执行单点状态机终态确认。
     */
    spin_lock_irq(&ep->object.lock);
    if (client->wait_entry.state == NOS_WAIT_STATE_WAITING) {
        (void)nos_wait_complete_locked(&client->wait_entry, -ETIMEDOUT, NULL);
    }
    rc = client->wait_entry.result;
    spin_unlock_irq(&ep->object.lock);

    if (timeout != NOS_WAIT_FOREVER) {
        timer_stop(&client->timer);
    }

    spin_lock_irq(&client->lock);
    if (client->flag == -ETIMEDOUT) {
        client->flag = 0;
    }
    task_clear_wchan(client);
    nos_wait_finish(&client->wait_entry);
    client->ipc_state = NOS_IPC_STATE_NONE;
    client->ipc_partner = NULL;
    spin_unlock_irq(&client->lock);

    if (rc < 0) {
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
    nos_timeout_t timeout;
    uint64_t deadline;
    bool boosted = false;
    uint8_t boost_prio = 0;
    int rc;

    if (ep == NULL || msg == NULL || server == NULL || server->pid == IDEL_TASK_PID) {
        return -EINVAL;
    }

    timeout = ipc_timeout_to_generic(timeout_ticks);

    spin_lock_irq(&server->lock);
    spin_lock_irq(&ep->object.lock);

    if (ep->object.closed) {
        spin_unlock_irq(&ep->object.lock);
        spin_unlock_irq(&server->lock);
        return -EPIPE;
    }

    if (!nos_wait_queue_empty(&ep->send_queue)) {
        /* 快路径：从 send_queue 取出最高优先级的等待发送者 */
        struct nos_wait_entry *client_entry = nos_wait_queue_first(&ep->send_queue);
        client = client_entry->task;

        /* 将 client 从 send_queue 移至 reply_queue 跟踪在途状态 */
        nos_wait_queue_del(&ep->send_queue, client_entry);
        list_add_tail(&client_entry->wait_node, &ep->reply_queue);
        client_entry->queue = NULL;

        client->ipc_state = NOS_IPC_STATE_REPLY_WAIT;
        client->ipc_partner = server;
        server->ipc_partner = client;

        ipc_transfer_caps(client, server, &client->ipc_msg);
        *msg = client->ipc_msg;

        /* 优先级继承：若客户端优先级高于服务端，提升服务端优先级 */
        if (client->current_priority < server->current_priority) {
            boosted = true;
            boost_prio = client->current_priority;
        }

        spin_unlock_irq(&ep->object.lock);
        spin_unlock_irq(&server->lock);

        if (boosted) {
            (void)task_pi_boost(server, boost_prio);
        }
        if (out_caller != NULL) {
            *out_caller = client;
        }
        return 0;
    }

    /* 慢路径：暂无客户端发送请求 */
    if (timeout == NOS_NO_WAIT) {
        spin_unlock_irq(&ep->object.lock);
        spin_unlock_irq(&server->lock);
        return -EAGAIN;
    }

    rc = nos_deadline_from_timeout(timeout, &deadline);
    if (rc < 0) {
        spin_unlock_irq(&ep->object.lock);
        spin_unlock_irq(&server->lock);
        return rc;
    }

    rc = nos_wait_prepare(&server->wait_entry, &ep->object, deadline);
    if (rc < 0) {
        spin_unlock_irq(&ep->object.lock);
        spin_unlock_irq(&server->lock);
        return rc;
    }

    task_set_wchan(server, "ipc_recv", (uintptr_t)ep);
    rc = task_hang_lock(server);
    if (rc < 0) {
        nos_wait_finish(&server->wait_entry);
        task_clear_wchan(server);
        spin_unlock_irq(&ep->object.lock);
        spin_unlock_irq(&server->lock);
        return rc;
    }

    server->flag = 0;
    server->ipc_state = NOS_IPC_STATE_RECV_WAIT;
    server->ipc_partner = NULL;
    nos_wait_queue_add_prio(&ep->recv_queue, &server->wait_entry);

    if (timeout != NOS_WAIT_FOREVER) {
        timer_start(&server->timer, (uint32_t)timeout);
    }

    spin_unlock_irq(&ep->object.lock);
    spin_unlock_irq(&server->lock);

    switch_task();

    /*
     * 服务端被唤醒后（可能由 client 发起 call 唤醒、endpoint 关闭唤醒，或由 timer 定时器超时唤醒）：
     * 在对象锁内执行单点状态机终态确认。
     */
    spin_lock_irq(&ep->object.lock);
    if (server->wait_entry.state == NOS_WAIT_STATE_WAITING) {
        (void)nos_wait_complete_locked(&server->wait_entry, -ETIMEDOUT, NULL);
    }
    rc = server->wait_entry.result;
    spin_unlock_irq(&ep->object.lock);

    if (timeout != NOS_WAIT_FOREVER) {
        timer_stop(&server->timer);
    }

    spin_lock_irq(&server->lock);
    if (server->flag == -ETIMEDOUT) {
        server->flag = 0;
    }
    task_clear_wchan(server);
    nos_wait_finish(&server->wait_entry);
    server->ipc_state = NOS_IPC_STATE_NONE;

    if (rc < 0) {
        server->ipc_partner = NULL;
        spin_unlock_irq(&server->lock);
        return rc;
    }

    *msg = server->ipc_msg;
    if (out_caller != NULL) {
        *out_caller = server->ipc_partner;
    }
    spin_unlock_irq(&server->lock);

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
    struct task_struct *target = NULL;
    struct nos_wake_batch batch;
    bool should_unboost = false;

    if (server == NULL || server->pid == IDEL_TASK_PID) {
        return -EINVAL;
    }

    target = (caller != NULL) ? caller : server->ipc_partner;

    /*
     * 若指定了回复内容，执行安全回复校验与状态交付
     */
    if (reply != NULL) {
        if (target == NULL || ep == NULL) {
            return -EINVAL;
        }

        nos_wake_batch_init(&batch);

        spin_lock_irq(&ep->object.lock);

        /*
         * 严格在 ep->object.lock 保护下校验 target 客户端：
         * 1. target 是否仍然存在于 ep->reply_queue 链表中；
         * 2. target 的等待状态是否仍为 NOS_WAIT_STATE_WAITING。
         * 若已超时或被取消，安全返回 -ETIMEDOUT，绝不写入已失效客户端内存 (UAF 防护)。
         */
        struct nos_wait_entry *entry, *target_entry = NULL;
        list_for_each_entry(entry, &ep->reply_queue, wait_node) {
            if (entry->task == target) {
                target_entry = entry;
                break;
            }
        }

        if (target_entry == NULL || target_entry->state != NOS_WAIT_STATE_WAITING) {
            bool is_closed = ep->object.closed;

            if (server->current_priority != server->init_priority) {
                should_unboost = true;
            }
            server->ipc_partner = NULL;
            spin_unlock_irq(&ep->object.lock);

            if (should_unboost) {
                (void)task_pi_unboost(server, TASK_PRIO_INVALID);
            }
            return is_closed ? -EPIPE : -ETIMEDOUT;
        }

        /* 目标客户端有效在途等待：复制回复消息并处理权能委托 */
        target->ipc_msg = *reply;
        ipc_transfer_caps(server, target, &target->ipc_msg);

        target->ipc_state = NOS_IPC_STATE_NONE;
        target->ipc_partner = NULL;
        server->ipc_partner = NULL;

        /* 完成客户端等待裁决并加入延后唤醒批次 */
        (void)nos_wait_complete_locked(&target->wait_entry, 0, &batch);

        if (server->current_priority != server->init_priority) {
            should_unboost = true;
        }

        spin_unlock_irq(&ep->object.lock);

        if (should_unboost) {
            (void)task_pi_unboost(server, TASK_PRIO_INVALID);
        }

        /* 在对象锁外安全分发延后唤醒批次 */
        nos_wake_batch_dispatch(&batch);
    }

    /* 若需要接收下一条请求，直接进入 nos_endpoint_recv */
    if (ep != NULL && next_req != NULL) {
        return nos_endpoint_recv(ep, next_req, out_next_caller, timeout_ticks);
    }

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

    rc = nos_endpoint_call(ep, badge, req, resp, timeout_ticks);
    nos_object_put(&ep->object);
    return rc;
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

    rc = nos_endpoint_recv(ep, msg, NULL, timeout_ticks);
    nos_object_put(&ep->object);
    return rc;
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

    if (ep_cap != NOS_HANDLE_INVALID) {
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

    rc = nos_endpoint_reply_wait(ep,
                                 task->ipc_partner,
                                 reply,
                                 next_req,
                                 NULL,
                                 timeout_ticks);
    if (ep != NULL) {
        nos_object_put(&ep->object);
    }
    return rc;
}
