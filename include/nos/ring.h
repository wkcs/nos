/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS AI-Native 异步零拷贝 RingIPC (SQ/CQ 共享内存环) 内核接口。
 * 特性：
 * - 单页双 Cache-Line 无伪共享设计（128B nos_ring_header + SQ[32] + CQ[32] <= 4KB）
 * - 16 槽位 Schema 契约白名单强制门禁
 * - 与 Agent Cell 五维硬预算（tool_calls / accel_ops）及 64B 强制审计环无缝联动
 * - 跨服务 Deadline Inheritance 截止期继承传播
 */

#ifndef __NOS_RING_IPC_H__
#define __NOS_RING_IPC_H__

#include <uapi/nos/types.h>
#include <uapi/nos/ai.h>
#include <kernel/spinlock.h>
#include <nos/object.h>
#include <nos/notification.h>

struct nos_vmo;
struct nos_cell;
struct nos_vm_space;
struct task_struct;
struct nos_ring;

typedef int (*nos_ring_handler_t)(struct nos_ring *ring,
                                  struct task_struct *caller,
                                  const struct nos_ring_sqe *sqe,
                                  struct nos_ring_cqe *out_cqe,
                                  void *ctx);

struct nos_ring_pending_slot {
    struct nos_ring_sqe sqe;
    bool                deadline_boosted;
};

struct nos_ring {
    struct nos_object             header;       /* 必须置于首字段，供 Capability Pinned Lookup */
    spinlock_t                    lock;
    struct nos_vmo               *ring_vmo;     /* 单页 DMA_TENSOR | IO_COHERENT 共享内存环 */
    struct nos_cell              *owner_cell;   /* 可选绑定的创建/扣费 Agent Cell */
    uint32_t                      sq_entries;   /* 2 的幂 (<= 32) */
    uint32_t                      cq_entries;   /* 2 的幂 (<= 32) */
    bool                          schema_filter_enabled;
    uint32_t                      schema_count;
    uint64_t                      schema_whitelist[NOS_RING_MAX_SCHEMAS];
    struct nos_notification       sq_notif;     /* 唤醒服务端消费 SQ */
    struct nos_notification       cq_notif;     /* 唤醒客户端收割 CQ */
    struct task_struct           *server_task;  /* 可选绑定工具代理线程（支持 Deadline Inheritance） */
    nos_ring_handler_t            kernel_handler;
    void                         *handler_ctx;
    uint32_t                      active_boosts;
    struct nos_ring_pending_slot  pending_queue[NOS_RING_DEFAULT_ENTRIES];
    uint32_t                      pending_head;
    uint32_t                      pending_tail;
    uint32_t                      pending_count;
};

struct nos_ring *nos_ring_create(const struct nos_ring_create_args *args,
                                 const uint64_t *schemas,
                                 struct nos_vm_space *map_space,
                                 struct nos_cell *charge_cell);

int nos_ring_destroy(struct nos_ring *ring);

int nos_ring_map(struct nos_ring *ring,
                 struct nos_vm_space *space,
                 uintptr_t user_vaddr,
                 struct nos_cell *charge_cell);

int nos_ring_bind_server(struct nos_ring *ring,
                         struct task_struct *server_task,
                         nos_ring_handler_t handler,
                         void *ctx);

int nos_ring_enter(struct nos_ring *ring,
                   struct task_struct *caller,
                   uint32_t to_submit,
                   uint32_t min_complete,
                   uint32_t flags,
                   uint32_t timeout_ticks);

int nos_ring_enter_with_cap(struct nos_ring *ring,
                            struct task_struct *caller,
                            nos_handle_t ring_handle,
                            uint32_t to_submit,
                            uint32_t min_complete,
                            uint32_t flags,
                            uint32_t timeout_ticks);

int nos_ring_server_pop(struct nos_ring *ring,
                        struct nos_ring_sqe *out_sqe,
                        uint32_t timeout_ticks);

int nos_ring_server_complete(struct nos_ring *ring,
                             const struct nos_ring_cqe *cqe);

#endif /* __NOS_RING_IPC_H__ */
