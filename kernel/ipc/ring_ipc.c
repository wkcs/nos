/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS RingIPC 异步提交/完成环内核核心实现：
 * 1. 无锁用户-内核共享单页环（128B 双 Cache-Line 头 + SQ/CQ）；
 * 2. 强类型 Schema 白名单过滤、Cell 5D 硬配额扣费、64B 不可伪造审计与零拷贝 VMO 切片校验；
 * 3. 与 sched_infer 协同的跨线程截止期继承 (Deadline Inheritance)。
 */

#define pr_fmt(fmt) "[RING]:%s[%d]:"fmt, __func__, __LINE__

#include <nos/ring.h>
#include <nos/vmo.h>
#include <nos/cell.h>
#include <nos/cap.h>
#include <nos/sched_infer.h>
#include <nos/hal_arch.h>
#include <uapi/nos/errno.h>
#include <uapi/nos/ipc.h>
#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <kernel/task.h>
#include <asm-generic/mmu.h>
#include <string.h>

#define NOS_RING_BATCH_SIZE 8U

__attribute__((weak)) int nos_accel_dispatch_ring_sqe(void *accel_obj,
                                                      struct task_struct *caller,
                                                      const struct nos_ring_sqe *sqe,
                                                      struct nos_ring_cqe *out_cqe)
{
    (void)caller;
    (void)sqe;
    (void)out_cqe;
    if (accel_obj != NULL) {
        (void)nos_object_put((struct nos_object *)accel_obj);
    }
    return -ENOSYS;
}

__attribute__((weak)) int nos_accel_cancel_by_obj(void *accel_obj, uint64_t job_id)
{
    (void)job_id;
    if (accel_obj != NULL) {
        (void)nos_object_put((struct nos_object *)accel_obj);
    }
    return -ENOSYS;
}

static nos_timeout_t ring_to_timeout(uint32_t timeout_ticks)
{
    if (timeout_ticks == NOS_IPC_TIMEOUT_NONBLOCK) {
        return NOS_NO_WAIT;
    }
    if (timeout_ticks == NOS_IPC_TIMEOUT_INFINITE) {
        return NOS_WAIT_FOREVER;
    }
    return (nos_timeout_t)timeout_ticks;
}

static bool ring_is_valid_count(uint32_t n)
{
    return (n >= 2U && n <= NOS_RING_DEFAULT_ENTRIES && (n & (n - 1U)) == 0U);
}

static struct nos_ring_header *ring_header_ptr(struct nos_ring *ring)
{
    if (ring == NULL || ring->ring_vmo == NULL ||
        ring->ring_vmo->page_count == 0U || ring->ring_vmo->pages[0] == NULL) {
        return NULL;
    }
    return (struct nos_ring_header *)ring->ring_vmo->pages[0]->kvaddr;
}

static void ring_release(struct nos_object *obj)
{
    struct nos_ring *ring = (struct nos_ring *)obj;

    if (ring == NULL) {
        return;
    }

    (void)nos_notification_close(&ring->sq_notif);
    (void)nos_notification_close(&ring->cq_notif);

    if (ring->server_task != NULL) {
        (void)task_put(ring->server_task);
        ring->server_task = NULL;
    }
    if (ring->ring_vmo != NULL) {
        (void)nos_object_close(&ring->ring_vmo->header);
        (void)nos_object_retire(&ring->ring_vmo->header);
        ring->ring_vmo = NULL;
    }
    if (ring->owner_cell != NULL) {
        (void)nos_object_put(&ring->owner_cell->header);
        ring->owner_cell = NULL;
    }

    kfree(ring);
}

static bool ring_schema_allowed(const struct nos_ring *ring, uint64_t schema_id)
{
    uint32_t i;

    if (!ring->schema_filter_enabled) {
        return true;
    }
    for (i = 0U; i < ring->schema_count; i++) {
        if (ring->schema_whitelist[i] == schema_id) {
            return true;
        }
    }
    return false;
}

static int ring_push_cqe_locked(struct nos_ring *ring, const struct nos_ring_cqe *cqe)
{
    struct nos_ring_header *hdr = ring_header_ptr(ring);
    struct nos_ring_cqe *cq_base;
    uint32_t cq_tail;
    uint32_t cq_head;
    uint32_t slot;

    if (hdr == NULL || cqe == NULL) {
        return -EINVAL;
    }

    cq_tail = __atomic_load_n(&hdr->cq_tail, __ATOMIC_RELAXED);
    cq_head = __atomic_load_n(&hdr->cq_head, __ATOMIC_ACQUIRE);
    if ((uint32_t)(cq_tail - cq_head) >= ring->cq_entries) {
        hdr->cq_overflow++;
        return -ENOSPC;
    }

    cq_base = (struct nos_ring_cqe *)((uint8_t *)hdr + hdr->cq_offset);
    slot = cq_tail & (ring->cq_entries - 1U);
    cq_base[slot] = *cqe;
    smp_wmb();
    __atomic_store_n(&hdr->cq_tail, cq_tail + 1U, __ATOMIC_RELEASE);
    hdr->total_completed++;
    return 0;
}

static void ring_push_immediate_error(struct nos_ring *ring, uint64_t req_id, int32_t err)
{
    struct nos_ring_cqe cqe;
    bool notify_cq = false;

    memset(&cqe, 0, sizeof(cqe));
    cqe.req_id = req_id;
    cqe.status = err;

    spin_lock_irq(&ring->lock);
    if (ring_push_cqe_locked(ring, &cqe) == 0) {
        notify_cq = true;
    }
    spin_unlock_irq(&ring->lock);

    if (notify_cq) {
        (void)nos_notify_signal_obj(&ring->cq_notif, 1ULL);
    }
}

static void ring_cdt_revoke_drain_cb(uint16_t cap_type, void *obj)
{
    struct nos_ring *ring;
    struct nos_ring_sqe canceled_sqes[NOS_RING_DEFAULT_ENTRIES];
    uint32_t cnt = 0U;
    struct task_struct *restore_srv = NULL;

    if (cap_type != NOS_CAP_RING || obj == NULL) {
        return;
    }

    ring = (struct nos_ring *)obj;
    spin_lock_irq(&ring->lock);
    while (ring->pending_count > 0U && cnt < NOS_RING_DEFAULT_ENTRIES) {
        canceled_sqes[cnt++] = ring->pending_queue[ring->pending_head].sqe;
        ring->pending_head = (ring->pending_head + 1U) % NOS_RING_DEFAULT_ENTRIES;
        ring->pending_count--;
    }
    if (ring->active_boosts > 0U) {
        ring->active_boosts = 0U;
        if (ring->server_task != NULL && task_get(ring->server_task)) {
            restore_srv = ring->server_task;
        }
    }
    for (uint32_t i = 0U; i < cnt; i++) {
        struct nos_ring_cqe cqe;
        memset(&cqe, 0, sizeof(cqe));
        cqe.req_id = canceled_sqes[i].req_id;
        cqe.status = -ECANCELED;
        (void)ring_push_cqe_locked(ring, &cqe);
    }
    spin_unlock_irq(&ring->lock);

    if (restore_srv != NULL) {
        sched_infer_restore_deadline(restore_srv);
        (void)task_put(restore_srv);
    }
    (void)nos_notify_signal_obj(&ring->sq_notif, 1ULL);
    (void)nos_notify_signal_obj(&ring->cq_notif, 1ULL);
}

static uint32_t g_ring_drain_registered = 0U;

struct nos_ring *nos_ring_create(const struct nos_ring_create_args *args,
                                 const uint64_t *schemas,
                                 struct nos_vm_space *map_space,
                                 struct nos_cell *charge_cell)
{
    struct nos_ring *ring;
    struct nos_vmo_create_args vmo_args;
    struct nos_ring_header *hdr;
    uint32_t sq_n;
    uint32_t cq_n;
    size_t total_bytes;

    if (args == NULL || args->schema_count > NOS_RING_MAX_SCHEMAS) {
        return NULL;
    }
    if (args->schema_count > 0U && schemas == NULL) {
        return NULL;
    }

    sq_n = (args->sq_entries != 0U) ? args->sq_entries : NOS_RING_DEFAULT_ENTRIES;
    cq_n = (args->cq_entries != 0U) ? args->cq_entries : NOS_RING_DEFAULT_ENTRIES;
    if (!ring_is_valid_count(sq_n) || !ring_is_valid_count(cq_n)) {
        return NULL;
    }

    total_bytes = sizeof(struct nos_ring_header) +
                  (size_t)sq_n * sizeof(struct nos_ring_sqe) +
                  (size_t)cq_n * sizeof(struct nos_ring_cqe);
    if (total_bytes > PAGE_SIZE) {
        return NULL;
    }

    if (charge_cell != NULL && !nos_object_get(&charge_cell->header)) {
        return NULL;
    }

    ring = kzalloc(sizeof(*ring), GFP_KERNEL);
    if (ring == NULL) {
        if (charge_cell != NULL) {
            (void)nos_object_put(&charge_cell->header);
        }
        return NULL;
    }

    memset(&vmo_args, 0, sizeof(vmo_args));
    vmo_args.size_bytes = PAGE_SIZE;
    vmo_args.kind = NOS_VMO_DMA_TENSOR;
    vmo_args.flags = NOS_VMO_FLAG_IO_COHERENT;

    ring->ring_vmo = nos_vmo_create(&vmo_args);
    if (ring->ring_vmo == NULL) {
        kfree(ring);
        if (charge_cell != NULL) {
            (void)nos_object_put(&charge_cell->header);
        }
        return NULL;
    }

    (void)nos_object_init(&ring->header, false);
    nos_object_set_release(&ring->header, ring_release);
    spin_lock_init(&ring->lock);
    (void)nos_notification_init(&ring->sq_notif);
    (void)nos_notification_init(&ring->cq_notif);

    ring->owner_cell = charge_cell;
    ring->sq_entries = sq_n;
    ring->cq_entries = cq_n;
    ring->schema_count = args->schema_count;
    ring->schema_filter_enabled = ((args->flags & NOS_RING_CREATE_SCHEMA_FILTER) != 0U) ||
                                  (args->schema_count > 0U);
    if (args->schema_count > 0U) {
        memcpy(ring->schema_whitelist, schemas, args->schema_count * sizeof(uint64_t));
    }

    hdr = ring_header_ptr(ring);
    memset(hdr, 0, PAGE_SIZE);
    hdr->magic = NOS_RING_MAGIC;
    hdr->version = 1U;
    hdr->flags = (uint16_t)args->flags;
    hdr->sq_entries = sq_n;
    hdr->cq_entries = cq_n;
    hdr->sq_offset = (uint32_t)sizeof(struct nos_ring_header);
    hdr->cq_offset = hdr->sq_offset + sq_n * (uint32_t)sizeof(struct nos_ring_sqe);
    smp_wmb();

    if (args->map_user_vaddr != 0ULL && map_space != NULL) {
        int rc = nos_ring_map(ring, map_space, (uintptr_t)args->map_user_vaddr, charge_cell);
        if (rc != 0) {
            (void)nos_ring_destroy(ring);
            return NULL;
        }
    }

    if (__atomic_exchange_n(&g_ring_drain_registered, 1U, __ATOMIC_ACQ_REL) == 0U) {
        nos_cap_register_revoke_drain_hook(ring_cdt_revoke_drain_cb);
    }

    return ring;
}

int nos_ring_destroy(struct nos_ring *ring)
{
    int rc;

    if (ring == NULL) {
        return -EINVAL;
    }

    (void)nos_notification_close(&ring->sq_notif);
    (void)nos_notification_close(&ring->cq_notif);
    (void)nos_object_close(&ring->header);
    rc = nos_object_retire(&ring->header);
    return (rc == -EINVAL) ? 0 : rc;
}

int nos_ring_map(struct nos_ring *ring,
                 struct nos_vm_space *space,
                 uintptr_t user_vaddr,
                 struct nos_cell *charge_cell)
{
    if (ring == NULL || ring->ring_vmo == NULL || space == NULL || user_vaddr == 0U) {
        return -EINVAL;
    }

    return nos_vmo_map(ring->ring_vmo,
                       space,
                       user_vaddr,
                       0U,
                       PAGE_SIZE,
                       HAL_VM_PROT_USER | HAL_VM_PROT_READ | HAL_VM_PROT_WRITE,
                       charge_cell);
}

int nos_ring_bind_server(struct nos_ring *ring,
                         struct task_struct *server_task,
                         nos_ring_handler_t handler,
                         void *ctx)
{
    struct task_struct *old_server;

    if (ring == NULL) {
        return -EINVAL;
    }
    if (server_task != NULL && !task_get(server_task)) {
        return -ESTALE;
    }

    spin_lock_irq(&ring->lock);
    old_server = ring->server_task;
    ring->server_task = server_task;
    ring->kernel_handler = handler;
    ring->handler_ctx = ctx;
    spin_unlock_irq(&ring->lock);

    if (old_server != NULL) {
        (void)task_put(old_server);
    }
    return 0;
}

static int ring_validate_vmo_slice(struct nos_cap_table *cap_tbl,
                                   const struct nos_ring_sqe *sqe)
{
    struct nos_vmo *vmo = NULL;
    nos_cap_rights_t req_rights = NOS_CAP_RIGHT_READ;
    int rc;

    if (sqe->vmo_cap == NOS_HANDLE_INVALID) {
        return 0;
    }
    if (cap_tbl == NULL) {
        return -EBADF;
    }
    if ((sqe->flags & NOS_RING_FLAG_ZERO_COPY_RO) == 0U &&
        (sqe->flags & NOS_RING_FLAG_MOVE_SLICE) != 0U) {
        req_rights |= NOS_CAP_RIGHT_WRITE;
    }

    rc = nos_cap_lookup(cap_tbl,
                        (nos_handle_t)sqe->vmo_cap,
                        NOS_CAP_VMO,
                        req_rights,
                        (void **)&vmo,
                        NULL);
    if (rc < 0) {
        return rc;
    }

    if (sqe->vmo_offset > (uint64_t)vmo->size_bytes ||
        (uint64_t)sqe->vmo_len > ((uint64_t)vmo->size_bytes - sqe->vmo_offset)) {
        (void)nos_object_put(&vmo->header);
        return -EINVAL;
    }

    (void)nos_object_put(&vmo->header);
    return 0;
}

static uint64_t ring_compute_abs_deadline(uint64_t deadline_ns)
{
    uint64_t now_ns = hal_timer_monotonic_ns();

    /* 小于 100 秒的值视为相对延迟偏移，大于等于则直接作为绝对单调截止期 */
    if (deadline_ns <= 100000000000ULL) {
        return now_ns + deadline_ns;
    }
    return (deadline_ns > now_ns) ? deadline_ns : (now_ns + 100000ULL);
}

static void ring_dispatch_one_sqe(struct nos_ring *ring,
                                  struct task_struct *caller,
                                  nos_handle_t ring_handle,
                                  const struct nos_ring_sqe *sqe)
{
    struct nos_cell *cell = NULL;
    struct nos_cap_table *cap_tbl = NULL;
    struct task_struct *server_ref = NULL;
    nos_ring_handler_t handler = NULL;
    void *handler_ctx = NULL;
    struct nos_ring_sqe dispatch_sqe = *sqe;
    bool boosted = false;
    int rc;

    if (caller != NULL) {
        cap_tbl = nos_task_active_cap_table(caller);
        cell = caller->cell;
    }
    if (cell == NULL) {
        cell = ring->owner_cell;
    }
    if (cap_tbl == NULL && cell != NULL) {
        cap_tbl = nos_cell_cap_table(cell);
    }

    /* 1. CNode schema_mask 与 Ring 实例 Schema 白名单双重校验 */
    if (!ring_schema_allowed(ring, sqe->schema_id) ||
        (ring_handle != NOS_HANDLE_INVALID && cap_tbl != NULL &&
         sqe->opcode != NOS_RING_OP_CANCEL &&
         nos_cap_check_schema(cap_tbl, ring_handle, sqe->schema_id) < 0)) {
        if (cell != NULL) {
            nos_cell_record_audit(cell,
                                  NOS_AUDIT_TOOL_CALL,
                                  sqe->schema_id,
                                  sqe->target_cap,
                                  -EACCES,
                                  sqe->req_id,
                                  0ULL);
        }
        ring_push_immediate_error(ring, sqe->req_id, -EACCES);
        return;
    }

    /* 1.5 NOS_RING_OP_CANCEL：优先从 pending_queue 摘除挂起 SQE，或向加速器转发取消 */
    if (sqe->opcode == NOS_RING_OP_CANCEL) {
        uint64_t target_req = (sqe->inline_arg[0] != 0ULL) ? sqe->inline_arg[0] : sqe->vmo_offset;
        struct task_struct *restore_srv = NULL;
        bool canceled_in_ring = false;

        spin_lock_irq(&ring->lock);
        for (uint32_t k = 0U; k < ring->pending_count; k++) {
            uint32_t idx = (ring->pending_head + k) % NOS_RING_DEFAULT_ENTRIES;
            if (ring->pending_queue[idx].sqe.req_id == target_req) {
                bool was_boosted = ring->pending_queue[idx].deadline_boosted;
                for (uint32_t m = k; m + 1U < ring->pending_count; m++) {
                    uint32_t dst_i = (ring->pending_head + m) % NOS_RING_DEFAULT_ENTRIES;
                    uint32_t src_i = (ring->pending_head + m + 1U) % NOS_RING_DEFAULT_ENTRIES;
                    ring->pending_queue[dst_i] = ring->pending_queue[src_i];
                }
                ring->pending_tail = (ring->pending_tail + NOS_RING_DEFAULT_ENTRIES - 1U) %
                                     NOS_RING_DEFAULT_ENTRIES;
                ring->pending_count--;
                if (was_boosted && ring->active_boosts > 0U) {
                    ring->active_boosts--;
                    if (ring->active_boosts == 0U &&
                        ring->server_task != NULL &&
                        task_get(ring->server_task)) {
                        restore_srv = ring->server_task;
                    }
                }
                {
                    struct nos_ring_cqe cqe_target;
                    struct nos_ring_cqe cqe_cancel;
                    memset(&cqe_target, 0, sizeof(cqe_target));
                    cqe_target.req_id = target_req;
                    cqe_target.status = -ECANCELED;
                    (void)ring_push_cqe_locked(ring, &cqe_target);

                    memset(&cqe_cancel, 0, sizeof(cqe_cancel));
                    cqe_cancel.req_id = sqe->req_id;
                    cqe_cancel.status = 0;
                    (void)ring_push_cqe_locked(ring, &cqe_cancel);
                }
                canceled_in_ring = true;
                break;
            }
        }
        spin_unlock_irq(&ring->lock);

        if (restore_srv != NULL) {
            sched_infer_restore_deadline(restore_srv);
            (void)task_put(restore_srv);
        }
        if (canceled_in_ring) {
            (void)nos_notify_signal_obj(&ring->cq_notif, 1ULL);
            return;
        }

        if (cap_tbl != NULL &&
            (sqe->target_cap != NOS_HANDLE_INVALID || sqe->vmo_cap != NOS_HANDLE_INVALID)) {
            void *accel_obj = NULL;
            nos_handle_t ah = (nos_handle_t)((sqe->target_cap != NOS_HANDLE_INVALID)
                                             ? sqe->target_cap : sqe->vmo_cap);
            if (nos_cap_lookup(cap_tbl, ah, NOS_CAP_ACCEL, NOS_CAP_RIGHT_SEND,
                               &accel_obj, NULL) == 0) {
                int cancel_rc = nos_accel_cancel_by_obj(accel_obj, target_req);
                ring_push_immediate_error(ring, sqe->req_id, cancel_rc);
                return;
            }
        }

        ring_push_immediate_error(ring, sqe->req_id, -ENOENT);
        return;
    }

    /* 2. Agent Cell 五维硬预算原子扣除（tool_calls / accel_ops） */
    if (cell != NULL) {
        if (sqe->opcode == NOS_RING_OP_ACCEL_JOB) {
            rc = nos_cell_charge_accel_op(cell, sqe->schema_id, sqe->target_cap);
        } else {
            rc = nos_cell_charge_tool_call(cell, sqe->schema_id, sqe->target_cap);
        }
        if (rc < 0) {
            ring_push_immediate_error(ring, sqe->req_id, rc);
            return;
        }
    }

    /* 3. 可选零拷贝 VMO 切片句柄及越界校验 */
    rc = ring_validate_vmo_slice(cap_tbl, sqe);
    if (rc < 0) {
        ring_push_immediate_error(ring, sqe->req_id, rc);
        return;
    }

    spin_lock_irq(&ring->lock);
    handler = ring->kernel_handler;
    handler_ctx = ring->handler_ctx;
    if (ring->server_task != NULL && task_get(ring->server_task)) {
        server_ref = ring->server_task;
    }
    spin_unlock_irq(&ring->lock);

    /* 4. 截止期继承传播 (Deadline Inheritance) */
    if ((sqe->flags & NOS_RING_FLAG_DEADLINE) != 0U &&
        sqe->deadline_ns > 0ULL &&
        server_ref != NULL) {
        if (sched_infer_boost_deadline(server_ref,
                                       ring_compute_abs_deadline(sqe->deadline_ns)) == 0) {
            boosted = true;
        }
    }

    /* 5. 直通内核句柄同步完成，或压入待办队列并唤醒异步服务线程 */
    if (handler != NULL) {
        struct nos_ring_cqe cqe;
        memset(&cqe, 0, sizeof(cqe));
        cqe.req_id = sqe->req_id;
        cqe.status = handler(ring, caller, sqe, &cqe, handler_ctx);

        spin_lock_irq(&ring->lock);
        (void)ring_push_cqe_locked(ring, &cqe);
        spin_unlock_irq(&ring->lock);

        if (boosted && server_ref != NULL) {
            sched_infer_restore_deadline(server_ref);
        }
        if (server_ref != NULL) {
            (void)task_put(server_ref);
        }
        (void)nos_notify_signal_obj(&ring->cq_notif, 1ULL);
        return;
    }

    if (sqe->opcode == NOS_RING_OP_ACCEL_JOB && cap_tbl != NULL) {
        void *accel_obj = NULL;
        int lookup_rc = -EBADF;
        if (sqe->target_cap != NOS_HANDLE_INVALID) {
            lookup_rc = nos_cap_lookup(cap_tbl,
                                       (nos_handle_t)sqe->target_cap,
                                       NOS_CAP_ACCEL,
                                       NOS_CAP_RIGHT_SEND,
                                       &accel_obj,
                                       NULL);
        }
        if (lookup_rc == 0) {
            struct nos_ring_cqe cqe;
            memset(&cqe, 0, sizeof(cqe));
            cqe.req_id = sqe->req_id;
            rc = nos_accel_dispatch_ring_sqe(accel_obj, caller, sqe, &cqe);
            if (cqe.status == 0 && rc < 0) {
                cqe.status = rc;
            }

            spin_lock_irq(&ring->lock);
            (void)ring_push_cqe_locked(ring, &cqe);
            spin_unlock_irq(&ring->lock);

            if (boosted && server_ref != NULL) {
                sched_infer_restore_deadline(server_ref);
            }
            if (server_ref != NULL) {
                (void)task_put(server_ref);
            }
            (void)nos_notify_signal_obj(&ring->cq_notif, 1ULL);
            return;
        }
        if (server_ref == NULL) {
            ring_push_immediate_error(ring, sqe->req_id, lookup_rc);
            return;
        }
    }

    /* 4.5 NOS_RING_FLAG_MOVE_SLICE：派发给异步服务前派生至服务端能力表并原子撤销源句柄 */
    if ((dispatch_sqe.flags & NOS_RING_FLAG_MOVE_SLICE) != 0U &&
        dispatch_sqe.vmo_cap != NOS_HANDLE_INVALID &&
        server_ref != NULL && cap_tbl != NULL) {
        struct nos_cap_table *dst_tbl = nos_task_active_cap_table(server_ref);
        nos_cap_rights_t xfer_rights = NOS_CAP_RIGHT_READ | NOS_CAP_RIGHT_MAP |
                                       ((dispatch_sqe.flags & NOS_RING_FLAG_ZERO_COPY_RO)
                                            ? 0U : NOS_CAP_RIGHT_WRITE);
        nos_handle_t new_vmo_h = NOS_HANDLE_INVALID;
        int xfer_rc = (dst_tbl != NULL)
                          ? nos_cap_derive(cap_tbl,
                                           (nos_handle_t)dispatch_sqe.vmo_cap,
                                           dst_tbl,
                                           xfer_rights,
                                           0U,
                                           &new_vmo_h)
                          : -EINVAL;
        if (xfer_rc < 0) {
            if (boosted) {
                sched_infer_restore_deadline(server_ref);
            }
            (void)task_put(server_ref);
            ring_push_immediate_error(ring, sqe->req_id, xfer_rc);
            return;
        }
        /* 仅撤销客户端源句柄槽位并把子节点重挂原父（避免吊销刚派发给服务端的派生句柄） */
        (void)nos_cap_revoke(cap_tbl, (nos_handle_t)dispatch_sqe.vmo_cap);
        dispatch_sqe.vmo_cap = (uint32_t)new_vmo_h;
    }

    spin_lock_irq(&ring->lock);
    if (ring->pending_count >= NOS_RING_DEFAULT_ENTRIES) {
        spin_unlock_irq(&ring->lock);
        if (boosted && server_ref != NULL) {
            sched_infer_restore_deadline(server_ref);
        }
        if (server_ref != NULL) {
            (void)task_put(server_ref);
        }
        ring_push_immediate_error(ring, sqe->req_id, -EBUSY);
        return;
    }

    ring->pending_queue[ring->pending_tail].sqe = dispatch_sqe;
    ring->pending_queue[ring->pending_tail].deadline_boosted = boosted;
    ring->pending_tail = (ring->pending_tail + 1U) % NOS_RING_DEFAULT_ENTRIES;
    ring->pending_count++;
    if (boosted) {
        ring->active_boosts++;
    }
    spin_unlock_irq(&ring->lock);

    if (server_ref != NULL) {
        (void)task_put(server_ref);
    }
    (void)nos_notify_signal_obj(&ring->sq_notif, 1ULL);
}

int nos_ring_enter_with_cap(struct nos_ring *ring,
                            struct task_struct *caller,
                            nos_handle_t ring_handle,
                            uint32_t to_submit,
                            uint32_t min_complete,
                            uint32_t flags,
                            uint32_t timeout_ticks)
{
    uint32_t submitted = 0U;

    if (ring == NULL) {
        return -EINVAL;
    }
    if (nos_object_is_closed(&ring->header)) {
        return -EPIPE;
    }

    if ((flags & NOS_RING_ENTER_SUBMIT) != 0U && to_submit > 0U) {
        while (submitted < to_submit) {
            struct nos_ring_sqe batch[NOS_RING_BATCH_SIZE];
            uint32_t batch_cnt = 0U;
            struct nos_ring_header *hdr;
            struct nos_ring_sqe *sq_base;
            uint32_t sq_tail;
            uint32_t sq_head;
            uint32_t avail;

            spin_lock_irq(&ring->lock);
            hdr = ring_header_ptr(ring);
            if (hdr == NULL || hdr->magic != NOS_RING_MAGIC) {
                spin_unlock_irq(&ring->lock);
                return -EINVAL;
            }
            sq_tail = __atomic_load_n(&hdr->sq_tail, __ATOMIC_ACQUIRE);
            sq_head = __atomic_load_n(&hdr->sq_head, __ATOMIC_RELAXED);
            avail = sq_tail - sq_head;
            if (avail > ring->sq_entries) {
                /* 防止恶意伪造超大 tail 引发越界/无限循环 */
                hdr->sq_dropped += avail;
                __atomic_store_n(&hdr->sq_head, sq_tail, __ATOMIC_RELEASE);
                spin_unlock_irq(&ring->lock);
                return -EINVAL;
            }
            if (avail == 0U) {
                spin_unlock_irq(&ring->lock);
                break;
            }

            sq_base = (struct nos_ring_sqe *)((uint8_t *)hdr + hdr->sq_offset);
            while (batch_cnt < NOS_RING_BATCH_SIZE &&
                   batch_cnt < avail &&
                   (submitted + batch_cnt) < to_submit) {
                uint32_t slot = (sq_head + batch_cnt) & (ring->sq_entries - 1U);
                batch[batch_cnt] = sq_base[slot];
                batch_cnt++;
            }
            __atomic_store_n(&hdr->sq_head, sq_head + batch_cnt, __ATOMIC_RELEASE);
            hdr->total_submitted += batch_cnt;
            spin_unlock_irq(&ring->lock);

            for (uint32_t i = 0U; i < batch_cnt; i++) {
                ring_dispatch_one_sqe(ring, caller, ring_handle, &batch[i]);
            }
            submitted += batch_cnt;
        }
    }

    if ((flags & NOS_RING_ENTER_WAIT_CQ) != 0U && min_complete > 0U) {
        nos_timeout_t wait_tmo = ring_to_timeout(timeout_ticks);
        for (;;) {
            struct nos_ring_header *hdr;
            uint32_t cq_avail;
            uint64_t dummy_bits = 0ULL;
            int wrc;

            spin_lock_irq(&ring->lock);
            hdr = ring_header_ptr(ring);
            cq_avail = __atomic_load_n(&hdr->cq_tail, __ATOMIC_ACQUIRE) -
                       __atomic_load_n(&hdr->cq_head, __ATOMIC_RELAXED);
            spin_unlock_irq(&ring->lock);

            if (cq_avail >= min_complete) {
                break;
            }
            if (wait_tmo == NOS_NO_WAIT) {
                return (submitted > 0U) ? (int)submitted : -EAGAIN;
            }

            wrc = nos_notify_wait_obj(&ring->cq_notif, &dummy_bits, wait_tmo);
            if (wrc < 0 && wrc != -EAGAIN) {
                return wrc;
            }
        }
    }

    return (int)submitted;
}

int nos_ring_enter(struct nos_ring *ring,
                   struct task_struct *caller,
                   uint32_t to_submit,
                   uint32_t min_complete,
                   uint32_t flags,
                   uint32_t timeout_ticks)
{
    return nos_ring_enter_with_cap(ring, caller, NOS_HANDLE_INVALID,
                                   to_submit, min_complete, flags, timeout_ticks);
}

int nos_ring_server_pop(struct nos_ring *ring,
                        struct nos_ring_sqe *out_sqe,
                        uint32_t timeout_ticks)
{
    nos_timeout_t wait_tmo;

    if (ring == NULL || out_sqe == NULL) {
        return -EINVAL;
    }
    if (!nos_object_get(&ring->header)) {
        return -EPIPE;
    }

    wait_tmo = ring_to_timeout(timeout_ticks);
    for (;;) {
        uint64_t dummy_bits = 0ULL;
        int wrc;

        if (nos_object_is_closed(&ring->header)) {
            (void)nos_object_put(&ring->header);
            return -EPIPE;
        }

        spin_lock_irq(&ring->lock);
        if (ring->pending_count > 0U) {
            *out_sqe = ring->pending_queue[ring->pending_head].sqe;
            ring->pending_head = (ring->pending_head + 1U) % NOS_RING_DEFAULT_ENTRIES;
            ring->pending_count--;
            spin_unlock_irq(&ring->lock);
            (void)nos_object_put(&ring->header);
            return 0;
        }
        spin_unlock_irq(&ring->lock);

        if (wait_tmo == NOS_NO_WAIT) {
            (void)nos_object_put(&ring->header);
            return -EAGAIN;
        }

        wrc = nos_notify_wait_obj(&ring->sq_notif, &dummy_bits, wait_tmo);
        if (wrc < 0 && wrc != -EAGAIN) {
            (void)nos_object_put(&ring->header);
            return wrc;
        }
    }
}

int nos_ring_server_complete(struct nos_ring *ring,
                             const struct nos_ring_cqe *cqe)
{
    struct task_struct *restore_srv = NULL;
    int rc;

    if (ring == NULL || cqe == NULL) {
        return -EINVAL;
    }

    spin_lock_irq(&ring->lock);
    rc = ring_push_cqe_locked(ring, cqe);
    if (rc == 0 && ring->active_boosts > 0U) {
        ring->active_boosts--;
        if (ring->active_boosts == 0U &&
            ring->server_task != NULL &&
            task_get(ring->server_task)) {
            restore_srv = ring->server_task;
        }
    }
    spin_unlock_irq(&ring->lock);

    if (restore_srv != NULL) {
        sched_infer_restore_deadline(restore_srv);
        (void)task_put(restore_srv);
    }

    if (rc == 0) {
        (void)nos_notify_signal_obj(&ring->cq_notif, 1ULL);
    }
    return rc;
}
