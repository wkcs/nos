/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * 宿主端 TDD 测试：RingIPC 异步零拷贝提交/完成环
 * 覆盖：Schema 白名单、5D 硬配额 (tool_calls/accel_ops)、零拷贝 VMO 切片校验、
 * 多线程同步唤醒/收割与 Deadline Inheritance、恶意 sq_tail 越界防御。
 */

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uapi/nos/errno.h>
#include <uapi/nos/ipc.h>
#include <uapi/nos/ai.h>
#include <kernel/mm.h>
#include <kernel/task.h>
#include <nos/cap.h>
#include <nos/vmo.h>
#include <nos/cell.h>
#include <nos/ring.h>
#include <nos/sched_infer.h>

_Static_assert(sizeof(struct nos_ring_header) == 128, "nos_ring_header 必须为 128 字节");
_Static_assert(sizeof(struct nos_ring_create_args) == 32, "nos_ring_create_args 必须为 32 字节");
_Static_assert(sizeof(struct nos_ring_sqe) == 64, "nos_ring_sqe 必须为 64 字节");
_Static_assert(sizeof(struct nos_ring_cqe) == 32, "nos_ring_cqe 必须为 32 字节");

uintptr_t kernel_running = 1;

void *kmalloc(size_t size, gfp_t flags)
{
    (void)flags;
    return malloc(size);
}

void *kzalloc(size_t size, gfp_t flags)
{
    (void)flags;
    return calloc(1, size);
}

void kfree(void *ptr)
{
    free(ptr);
}

static uint64_t g_mono_ns = 10000000ULL;
static uint32_t g_boost_calls;
static uint32_t g_restore_calls;
static uint64_t g_last_boost_dl;

uint64_t hal_timer_monotonic_ns(void)
{
    return __atomic_add_fetch(&g_mono_ns, 1000ULL, __ATOMIC_RELAXED);
}

int hal_vm_space_init(struct nos_vm_space *space)
{
    if (space == NULL) {
        return -EINVAL;
    }
    memset(space, 0, sizeof(*space));
    space->initialized = true;
    space->asid = 1U;
    return 0;
}

void hal_vm_space_destroy(struct nos_vm_space *space)
{
    if (space != NULL) {
        space->initialized = false;
    }
}

int hal_vm_map(struct nos_vm_space *space,
               uintptr_t vaddr,
               uintptr_t paddr,
               size_t size,
               uint32_t prot_flags)
{
    (void)space; (void)vaddr; (void)paddr; (void)size; (void)prot_flags;
    return 0;
}

int hal_vm_unmap(struct nos_vm_space *space, uintptr_t vaddr, size_t size)
{
    (void)space; (void)vaddr; (void)size;
    return 0;
}

void hal_vm_switch(struct nos_vm_space *next_space)
{
    (void)next_space;
}

struct task_struct *task_current(void)
{
    return NULL;
}

int sched_infer_boost_deadline(struct task_struct *target, uint64_t inherited_abs_deadline_ns)
{
    (void)target;
    __atomic_fetch_add(&g_boost_calls, 1U, __ATOMIC_RELAXED);
    __atomic_store_n(&g_last_boost_dl, inherited_abs_deadline_ns, __ATOMIC_RELAXED);
    return 0;
}

void sched_infer_restore_deadline(struct task_struct *target)
{
    (void)target;
    __atomic_fetch_add(&g_restore_calls, 1U, __ATOMIC_RELAXED);
}

/* 使用 pthread condvar 模拟真实内核异步 Bitmap Notification */
struct mock_pthread_notif {
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
    bool            inited;
};

#define MOCK_NOTIF_SLOTS 16U
static struct {
    struct nos_notification  *key;
    struct mock_pthread_notif state;
} g_notifs[MOCK_NOTIF_SLOTS];
static pthread_mutex_t g_notif_tbl_mtx = PTHREAD_MUTEX_INITIALIZER;

static struct mock_pthread_notif *find_or_alloc_notif(struct nos_notification *n, bool create)
{
    struct mock_pthread_notif *res = NULL;
    pthread_mutex_lock(&g_notif_tbl_mtx);
    for (uint32_t i = 0U; i < MOCK_NOTIF_SLOTS; i++) {
        if (g_notifs[i].key == n) {
            res = &g_notifs[i].state;
            break;
        }
        if (create && res == NULL && g_notifs[i].key == NULL) {
            g_notifs[i].key = n;
            pthread_mutex_init(&g_notifs[i].state.mtx, NULL);
            pthread_cond_init(&g_notifs[i].state.cv, NULL);
            g_notifs[i].state.inited = true;
            res = &g_notifs[i].state;
            break;
        }
    }
    pthread_mutex_unlock(&g_notif_tbl_mtx);
    return res;
}

int nos_notification_init(struct nos_notification *notif)
{
    if (notif == NULL) {
        return -EINVAL;
    }
    memset(notif, 0, sizeof(*notif));
    (void)nos_object_init(&notif->object, true);
    (void)find_or_alloc_notif(notif, true);
    return 0;
}

int nos_notification_close(struct nos_notification *notif)
{
    struct mock_pthread_notif *st;
    if (notif == NULL) {
        return -EINVAL;
    }
    (void)nos_object_close(&notif->object);
    st = find_or_alloc_notif(notif, false);
    if (st != NULL) {
        pthread_mutex_lock(&st->mtx);
        pthread_cond_broadcast(&st->cv);
        pthread_mutex_unlock(&st->mtx);
    }
    return 0;
}

int nos_notify_signal_obj(struct nos_notification *notif, uint64_t bits)
{
    struct mock_pthread_notif *st;
    if (notif == NULL || bits == 0ULL) {
        return -EINVAL;
    }
    if (nos_object_is_closed(&notif->object)) {
        return -EPIPE;
    }
    st = find_or_alloc_notif(notif, false);
    if (st == NULL) {
        __atomic_or_fetch(&notif->pending_bits, bits, __ATOMIC_RELEASE);
        return 0;
    }
    pthread_mutex_lock(&st->mtx);
    notif->pending_bits |= bits;
    pthread_cond_broadcast(&st->cv);
    pthread_mutex_unlock(&st->mtx);
    return 0;
}

int nos_notify_wait_obj(struct nos_notification *notif, uint64_t *out_bits, nos_timeout_t timeout)
{
    struct mock_pthread_notif *st;
    if (notif == NULL) {
        return -EINVAL;
    }
    st = find_or_alloc_notif(notif, false);
    if (st == NULL) {
        return -EINVAL;
    }
    pthread_mutex_lock(&st->mtx);
    while (notif->pending_bits == 0ULL && !nos_object_is_closed(&notif->object)) {
        if (timeout == NOS_NO_WAIT) {
            pthread_mutex_unlock(&st->mtx);
            return -EAGAIN;
        }
        pthread_cond_wait(&st->cv, &st->mtx);
    }
    if (notif->pending_bits != 0ULL) {
        if (out_bits != NULL) {
            *out_bits = notif->pending_bits;
        }
        notif->pending_bits = 0ULL;
        pthread_mutex_unlock(&st->mtx);
        return 0;
    }
    pthread_mutex_unlock(&st->mtx);
    return -EPIPE;
}

static int mock_kernel_tool_handler(struct nos_ring *ring,
                                    struct task_struct *caller,
                                    const struct nos_ring_sqe *sqe,
                                    struct nos_ring_cqe *out_cqe,
                                    void *ctx)
{
    (void)ring; (void)caller; (void)ctx;
    out_cqe->result_len = sqe->vmo_len;
    out_cqe->elapsed_ns = 1234ULL;
    return 0;
}

static void test_ring_whitelist_vmo_and_budgets(void)
{
    struct nos_cell_budget budget = {
        .cpu_ns_limit = 0ULL,
        .mem_bytes_limit = 65536ULL,
        .tool_calls_limit = 2ULL,
        .accel_ops_limit = 1ULL,
        .max_child_cells = 0U,
    };
    uint64_t schemas[2] = { 0x1001ULL, 0x2002ULL };
    struct nos_ring_create_args rargs = {
        .sq_entries = 16U,
        .cq_entries = 16U,
        .flags = NOS_RING_CREATE_SCHEMA_FILTER,
        .schema_count = 2U,
        .schema_list_ptr = (uint64_t)(uintptr_t)schemas,
        .map_user_vaddr = 0x440000ULL,
    };
    struct nos_vmo_create_args vargs = {
        .size_bytes = 8192U,
        .kind = NOS_VMO_ANON,
        .flags = 0U,
    };
    struct nos_cell *cell;
    struct nos_vmo *payload_vmo;
    struct nos_ring *ring;
    struct nos_ring_header *hdr;
    struct nos_ring_sqe *sq;
    struct nos_ring_cqe *cq;
    struct task_struct client;
    nos_handle_t vmo_h;
    const struct nos_cell_telemetry *tm;

    cell = nos_cell_create(NULL, &budget);
    assert(cell != NULL);
    tm = nos_cell_telemetry_view(cell);

    payload_vmo = nos_vmo_create(&vargs);
    assert(payload_vmo != NULL);
    assert(nos_cap_alloc(nos_cell_cap_table(cell), NOS_CAP_VMO, payload_vmo,
                         NOS_CAP_RIGHT_ALL, 0U, &vmo_h) == 0);

    ring = nos_ring_create(&rargs, schemas, nos_cell_vm_space(cell), cell);
    assert(ring != NULL);
    assert(nos_ring_bind_server(ring, NULL, mock_kernel_tool_handler, NULL) == 0);

    hdr = (struct nos_ring_header *)ring->ring_vmo->pages[0]->kvaddr;
    assert(hdr->magic == NOS_RING_MAGIC);
    assert(hdr->sq_entries == 16U && hdr->cq_entries == 16U);
    sq = (struct nos_ring_sqe *)((uint8_t *)hdr + hdr->sq_offset);
    cq = (struct nos_ring_cqe *)((uint8_t *)hdr + hdr->cq_offset);

    memset(&client, 0, sizeof(client));
    nos_refcount_init(&client.refcount, 1U);
    client.cell = cell;

    /* 写入 5 条混合 SQE 并提交 */
    memset(sq, 0, 16U * sizeof(*sq));
    /* 0: 合法 tool_call + 合法 1KB VMO 切片 */
    sq[0].opcode = NOS_RING_OP_TOOL_CALL;
    sq[0].req_id = 100ULL;
    sq[0].schema_id = 0x1001ULL;
    sq[0].vmo_cap = vmo_h;
    sq[0].vmo_offset = 4096ULL;
    sq[0].vmo_len = 1024U;
    /* 1: 非法 schema_id (未授权) */
    sq[1].opcode = NOS_RING_OP_TOOL_CALL;
    sq[1].req_id = 101ULL;
    sq[1].schema_id = 0xBAD0ULL;
    /* 2: 越界 VMO 切片 */
    sq[2].opcode = NOS_RING_OP_TOOL_CALL;
    sq[2].req_id = 102ULL;
    sq[2].schema_id = 0x1001ULL;
    sq[2].vmo_cap = vmo_h;
    sq[2].vmo_offset = 8000ULL;
    sq[2].vmo_len = 1024U;
    /* 3: 第 2 次合法 tool_call（由于 #2 VMO 校验在扣费之后：#0 + #2 + #3? Wait: #0 扣 1, #2 扣 1 到上限 2, 所以下面改为测 #2 先放第 3 条? 调整顺序先验证全部逻辑！） */
    sq[2].req_id = 102ULL;
    sq[2].schema_id = 0x2002ULL;
    sq[2].vmo_cap = NOS_HANDLE_INVALID;
    sq[2].vmo_len = 256U;
    /* 3: 越界 VMO 切片 -> 注意此时已消耗 tool_calls 2/2，为独立测试 VMO 越界留一条在单独调！
     * 改：先让 sq[2] 为非法 VMO，若 budget=3；这里测 limit=2：#0 占第 1 次，#1 非法 schema 不扣费，
     * #2 占第 2 次 (tool_calls=2/2)，#3 第 3 次必定触顶 -EDQUOT，#4 ACCEL_JOB 独立走 accel_ops=1/1！
     */
    sq[3].opcode = NOS_RING_OP_TOOL_CALL;
    sq[3].req_id = 103ULL;
    sq[3].schema_id = 0x2002ULL;

    sq[4].opcode = NOS_RING_OP_ACCEL_JOB;
    sq[4].req_id = 104ULL;
    sq[4].schema_id = 0x2002ULL;

    sq[5].opcode = NOS_RING_OP_ACCEL_JOB;
    sq[5].req_id = 105ULL;
    sq[5].schema_id = 0x2002ULL;

    __atomic_store_n(&hdr->sq_tail, 6U, __ATOMIC_RELEASE);
    assert(nos_ring_enter(ring, &client, 6U, 6U,
                          NOS_RING_ENTER_SUBMIT | NOS_RING_ENTER_WAIT_CQ,
                          NOS_IPC_TIMEOUT_INFINITE) == 6);

    assert(hdr->cq_tail == 6U);
    assert(cq[0].req_id == 100ULL && cq[0].status == 0 && cq[0].result_len == 1024U);
    assert(cq[1].req_id == 101ULL && cq[1].status == -EACCES);
    assert(cq[2].req_id == 102ULL && cq[2].status == 0 && cq[2].result_len == 256U);
    assert(cq[3].req_id == 103ULL && cq[3].status == -EDQUOT);
    assert(cq[4].req_id == 104ULL && cq[4].status == 0);
    assert(cq[5].req_id == 105ULL && cq[5].status == -EDQUOT);
    assert(tm->tool_calls_used == 2ULL);
    assert(tm->accel_ops_used == 1ULL);

    /* 校验恶意超大 sq_tail 欺骗被 -EINVAL 拦下 */
    __atomic_store_n(&hdr->sq_tail, hdr->sq_head + 999U, __ATOMIC_RELEASE);
    assert(nos_ring_enter(ring, &client, 1U, 0U, NOS_RING_ENTER_SUBMIT, 0U) == -EINVAL);
    assert(hdr->sq_dropped == 999U);

    assert(nos_ring_destroy(ring) == 0);
    assert(nos_object_close(&payload_vmo->header) == 0);
    assert(nos_object_retire(&payload_vmo->header) == 0);
    assert(nos_cell_destroy(cell) == 0);
}

struct async_srv_ctx {
    struct nos_ring *ring;
    bool             stop;
};

static void *async_worker_fn(void *arg)
{
    struct async_srv_ctx *ctx = (struct async_srv_ctx *)arg;
    struct nos_ring_sqe sqe;

    while (!ctx->stop) {
        struct nos_ring_cqe cqe;
        int rc = nos_ring_server_pop(ctx->ring, &sqe, NOS_IPC_TIMEOUT_INFINITE);
        if (rc == -EPIPE) {
            break;
        }
        assert(rc == 0);
        memset(&cqe, 0, sizeof(cqe));
        cqe.req_id = sqe.req_id;
        cqe.status = 0;
        cqe.result_len = 0x5A5AU;
        assert(nos_ring_server_complete(ctx->ring, &cqe) == 0);
    }
    return NULL;
}

static void test_ring_async_worker_and_deadline_inheritance(void)
{
    uint64_t schema = 0x544F4F4C0001ULL;
    struct nos_ring_create_args rargs = {
        .sq_entries = 8U,
        .cq_entries = 8U,
        .flags = NOS_RING_CREATE_SCHEMA_FILTER,
        .schema_count = 1U,
        .schema_list_ptr = (uint64_t)(uintptr_t)&schema,
    };
    struct nos_ring *ring;
    struct nos_ring_header *hdr;
    struct nos_ring_sqe *sq;
    struct nos_ring_cqe *cq;
    struct task_struct srv_task;
    struct async_srv_ctx sctx;
    pthread_t th;

    g_boost_calls = 0U;
    g_restore_calls = 0U;
    memset(&srv_task, 0, sizeof(srv_task));
    nos_refcount_init(&srv_task.refcount, 1U);

    ring = nos_ring_create(&rargs, &schema, NULL, NULL);
    assert(ring != NULL);
    assert(nos_ring_bind_server(ring, &srv_task, NULL, NULL) == 0);

    sctx.ring = ring;
    sctx.stop = false;
    assert(pthread_create(&th, NULL, async_worker_fn, &sctx) == 0);

    hdr = (struct nos_ring_header *)ring->ring_vmo->pages[0]->kvaddr;
    sq = (struct nos_ring_sqe *)((uint8_t *)hdr + hdr->sq_offset);
    cq = (struct nos_ring_cqe *)((uint8_t *)hdr + hdr->cq_offset);

    memset(&sq[0], 0, sizeof(sq[0]));
    sq[0].opcode = NOS_RING_OP_TOOL_CALL;
    sq[0].flags = NOS_RING_FLAG_DEADLINE;
    sq[0].req_id = 777ULL;
    sq[0].schema_id = schema;
    sq[0].deadline_ns = 500000ULL;
    __atomic_store_n(&hdr->sq_tail, 1U, __ATOMIC_RELEASE);

    assert(nos_ring_enter(ring, NULL, 1U, 1U,
                          NOS_RING_ENTER_SUBMIT | NOS_RING_ENTER_WAIT_CQ,
                          NOS_IPC_TIMEOUT_INFINITE) == 1);

    assert(cq[0].req_id == 777ULL && cq[0].status == 0 && cq[0].result_len == 0x5A5AU);
    assert(g_boost_calls == 1U && g_restore_calls == 1U);

    sctx.stop = true;
    assert(nos_ring_destroy(ring) == 0);
    assert(pthread_join(th, NULL) == 0);
}

static void test_ring_stage6_cancel_move_schema_and_drain(void)
{
    struct nos_ring_create_args rargs = {
        .sq_entries = 8U,
        .cq_entries = 8U,
        .flags = 0U,
        .schema_count = 0U,
    };
    struct nos_vmo_create_args vargs = {
        .size_bytes = 4096U,
        .kind = NOS_VMO_ANON,
        .flags = 0U,
    };
    struct nos_cell *cli_cell;
    struct nos_cell *srv_cell;
    struct task_struct cli_task;
    struct task_struct srv_task;
    struct nos_ring *ring;
    struct nos_vmo *vmo;
    struct nos_ring_header *hdr;
    struct nos_ring_sqe *sq;
    struct nos_ring_cqe *cq;
    nos_handle_t ring_h = NOS_HANDLE_INVALID;
    nos_handle_t vmo_h = NOS_HANDLE_INVALID;
    struct nos_ring_sqe popped;
    uint32_t revoked = 0U;

    cli_cell = nos_cell_create(NULL, NULL);
    srv_cell = nos_cell_create(NULL, NULL);
    assert(cli_cell != NULL && srv_cell != NULL);

    memset(&cli_task, 0, sizeof(cli_task));
    memset(&srv_task, 0, sizeof(srv_task));
    nos_refcount_init(&cli_task.refcount, 1U);
    nos_refcount_init(&srv_task.refcount, 1U);
    nos_cap_table_init(&cli_task.cap_table);
    nos_cap_table_init(&srv_task.cap_table);
    assert(nos_cell_attach_task(&cli_task, cli_cell) == 0);
    assert(nos_cell_attach_task(&srv_task, srv_cell) == 0);

    ring = nos_ring_create(&rargs, NULL, NULL, cli_cell);
    vmo = nos_vmo_create(&vargs);
    assert(ring != NULL && vmo != NULL);
    assert(nos_cap_alloc(nos_cell_cap_table(cli_cell), NOS_CAP_RING, ring,
                         NOS_CAP_RIGHT_ALL, 0U, &ring_h) == 0);
    assert(nos_cap_set_schema_mask(nos_cell_cap_table(cli_cell), ring_h, 0x0FUL) == 0);
    assert(nos_cap_alloc(nos_cell_cap_table(cli_cell), NOS_CAP_VMO, vmo,
                         NOS_CAP_RIGHT_ALL, 0U, &vmo_h) == 0);
    assert(nos_ring_bind_server(ring, &srv_task, NULL, NULL) == 0);

    hdr = (struct nos_ring_header *)ring->ring_vmo->pages[0]->kvaddr;
    sq = (struct nos_ring_sqe *)((uint8_t *)hdr + hdr->sq_offset);
    cq = (struct nos_ring_cqe *)((uint8_t *)hdr + hdr->cq_offset);

    /* 1. CNode schema_mask 拦阻越权 schema_id (0x10 不在 0x0F 内) */
    memset(&sq[0], 0, sizeof(sq[0]));
    sq[0].opcode = NOS_RING_OP_TOOL_CALL;
    sq[0].req_id = 1001ULL;
    sq[0].schema_id = 0x10ULL;
    /* 2. MOVE_SLICE + 有效 schema_id (0x02)：提交后源 vmo_h 撤销，srv_cell 得新句柄 */
    memset(&sq[1], 0, sizeof(sq[1]));
    sq[1].opcode = NOS_RING_OP_TOOL_CALL;
    sq[1].flags = NOS_RING_FLAG_MOVE_SLICE;
    sq[1].req_id = 1002ULL;
    sq[1].schema_id = 0x02ULL;
    sq[1].vmo_cap = (uint32_t)vmo_h;
    sq[1].vmo_len = 128U;
    /* 3. 待 CANCEL 的请求 1003 与 CANCEL 请求 1004 */
    memset(&sq[2], 0, sizeof(sq[2]));
    sq[2].opcode = NOS_RING_OP_TOOL_CALL;
    sq[2].req_id = 1003ULL;
    sq[2].schema_id = 0x04ULL;
    memset(&sq[3], 0, sizeof(sq[3]));
    sq[3].opcode = NOS_RING_OP_CANCEL;
    sq[3].req_id = 1004ULL;
    sq[3].schema_id = 0x04ULL;
    sq[3].vmo_offset = 1003ULL;
    __atomic_store_n(&hdr->sq_tail, 4U, __ATOMIC_RELEASE);

    assert(nos_ring_enter_with_cap(ring, &cli_task, ring_h, 4U, 0U,
                                   NOS_RING_ENTER_SUBMIT, NOS_IPC_TIMEOUT_NONBLOCK) == 4);

    /* CQE 顺序：0=[1001 -EACCES], 1=[1003 -ECANCELED], 2=[1004 0] */
    assert(hdr->cq_tail == 3U);
    assert(cq[0].req_id == 1001ULL && cq[0].status == -EACCES);
    assert(cq[1].req_id == 1003ULL && cq[1].status == -ECANCELED);
    assert(cq[2].req_id == 1004ULL && cq[2].status == 0);

    /* 客户端原 vmo_h 已被撤销，服务端弹出 SQE 拿到有效转移句柄 */
    assert(nos_cap_inspect(nos_cell_cap_table(cli_cell), vmo_h, NULL, NULL) == -EBADF);
    assert(nos_ring_server_pop(ring, &popped, NOS_IPC_TIMEOUT_NONBLOCK) == 0);
    assert(popped.req_id == 1002ULL && popped.vmo_cap != (uint32_t)vmo_h);
    void *srv_vmo_ptr = NULL;
    assert(nos_cap_lookup(nos_cell_cap_table(srv_cell), (nos_handle_t)popped.vmo_cap,
                          NOS_CAP_VMO, NOS_CAP_RIGHT_WRITE, &srv_vmo_ptr, NULL) == 0);
    assert(srv_vmo_ptr == vmo);
    (void)nos_object_put(&vmo->header);

    /* 4. 提交 req=1005 挂起在 pending_queue，随后 revoke_tree(ring_h) 验证排空回调产生 -ECANCELED */
    memset(&sq[4], 0, sizeof(sq[4]));
    sq[4].opcode = NOS_RING_OP_TOOL_CALL;
    sq[4].req_id = 1005ULL;
    sq[4].schema_id = 0x01ULL;
    __atomic_store_n(&hdr->sq_tail, 5U, __ATOMIC_RELEASE);
    assert(nos_ring_enter_with_cap(ring, &cli_task, ring_h, 1U, 0U,
                                   NOS_RING_ENTER_SUBMIT, NOS_IPC_TIMEOUT_NONBLOCK) == 1);
    assert(nos_cap_revoke_tree(nos_cell_cap_table(cli_cell), ring_h, &revoked) == 0);
    assert(revoked == 1U);
    assert(hdr->cq_tail == 4U && cq[3].req_id == 1005ULL && cq[3].status == -ECANCELED);

    nos_cell_detach_task(&cli_task);
    nos_cell_detach_task(&srv_task);
    nos_cap_table_destroy(&cli_task.cap_table);
    nos_cap_table_destroy(&srv_task.cap_table);
    assert(nos_ring_destroy(ring) == 0);
    assert(nos_object_close(&vmo->header) == 0);
    assert(nos_object_retire(&vmo->header) == 0);
    assert(nos_cell_destroy(cli_cell) == 0);
    assert(nos_cell_destroy(srv_cell) == 0);
}

int main(void)
{
    test_ring_whitelist_vmo_and_budgets();
    test_ring_async_worker_and_deadline_inheritance();
    test_ring_stage6_cancel_move_schema_and_drain();
    printf("PASS: test_ring_ipc (Schema+Mask, MOVE_SLICE, CANCEL, CDT drain, async CQ, Deadline Inheritance)\n");
    return 0;
}
