/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS Microkernel Synchronous Endpoint IPC & Asynchronous Notification
 */

#ifndef __NOS_IPC_H__
#define __NOS_IPC_H__

#include <uapi/nos/types.h>
#include <uapi/nos/ipc.h>
#include <kernel/list.h>
#include <kernel/spinlock.h>

struct task_struct;

/* Per-task IPC blocking state */
enum nos_ipc_state {
    NOS_IPC_STATE_NONE = 0,
    NOS_IPC_STATE_SEND_WAIT,
    NOS_IPC_STATE_REPLY_WAIT,
    NOS_IPC_STATE_RECV_WAIT,
    NOS_IPC_STATE_NOTIFY_WAIT,
};

/* Synchronous Rendezvous IPC Endpoint */
struct nos_endpoint {
    spinlock_t       lock;
    struct list_head send_queue; /* Clients waiting in nos_ipc_call to deliver request */
    struct list_head recv_queue; /* Servers waiting in nos_ipc_recv / nos_ipc_reply_wait */
    uint32_t         id;
    bool             closed;
};

/* Asynchronous Bitmask Notification Object */
struct nos_notification {
    spinlock_t          lock;
    uint64_t            pending_bits;
    struct list_head    wait_queue;
    bool                closed;
};

/* Endpoint lifecycle */
int nos_endpoint_init(struct nos_endpoint *ep, uint32_t id);
struct nos_endpoint *nos_endpoint_create(nos_handle_t *out_cap);
void nos_endpoint_close(struct nos_endpoint *ep);
void nos_endpoint_destroy(struct nos_endpoint *ep);

/* Synchronous Endpoint IPC operations (object pointer level) */
int nos_endpoint_call(struct nos_endpoint *ep,
                      uint32_t badge,
                      const struct nos_ipc_msg *req,
                      struct nos_ipc_msg *resp,
                      uint32_t timeout_ticks);

int nos_endpoint_recv(struct nos_endpoint *ep,
                      struct nos_ipc_msg *msg,
                      struct task_struct **out_caller,
                      uint32_t timeout_ticks);

int nos_endpoint_reply_wait(struct nos_endpoint *ep,
                            struct task_struct *caller,
                            const struct nos_ipc_msg *reply,
                            struct nos_ipc_msg *next_req,
                            struct task_struct **out_next_caller,
                            uint32_t timeout_ticks);

/* Capability-handle level synchronous IPC */
int nos_ipc_call(nos_handle_t ep_cap,
                 const struct nos_ipc_msg *req,
                 struct nos_ipc_msg *resp,
                 uint32_t timeout_ticks);

int nos_ipc_recv(nos_handle_t ep_cap,
                 struct nos_ipc_msg *msg,
                 uint32_t timeout_ticks);

int nos_ipc_reply_wait(nos_handle_t ep_cap,
                       const struct nos_ipc_msg *reply,
                       struct nos_ipc_msg *next_req,
                       uint32_t timeout_ticks);

/* Notification lifecycle & operations */
int nos_notification_init(struct nos_notification *notif);
struct nos_notification *nos_notification_create(nos_handle_t *out_cap);
void nos_notification_destroy(struct nos_notification *notif);

int nos_notify_signal_obj(struct nos_notification *notif, uint64_t bits);
int nos_notify_wait_obj(struct nos_notification *notif,
                        uint64_t *out_bits,
                        uint32_t timeout_ticks);

int nos_notify_signal(nos_handle_t notif_cap, uint64_t bits);
int nos_notify_wait(nos_handle_t notif_cap,
                    uint64_t *out_bits,
                    uint32_t timeout_ticks);

#endif /* __NOS_IPC_H__ */
