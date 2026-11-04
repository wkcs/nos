/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * 宿主端测试：微内核同步 Endpoint IPC 事务与安全 Reply 机制
 * 覆盖：
 * 1. 正常 Client-Server 同步调用与优先级继承；
 * 2. 客户端超时时，服务端回复安全返回 -ETIMEDOUT，不写已失效内存；
 * 3. Endpoint 关闭时，reply_queue 中等待回复的客户端被安全唤醒返回 -EPIPE；
 * 4. Capability 查找递增引用，并发销毁不引发悬空访问。
 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <uapi/nos/errno.h>
#include <uapi/nos/ipc.h>
#include <kernel/spinlock.h>
#include <kernel/task.h>
#include <kernel/timer.h>
#include <kernel/printk.h>
#include <nos/refcount.h>
#include <nos/object.h>
#include <nos/timeout.h>
#include <nos/wait.h>
#include <nos/ipc.h>
#include <nos/cap.h>

/* 静态断言校验 64 字节 ABI 契约 */
_Static_assert(sizeof(struct nos_ipc_msg) == 64, "nos_ipc_msg ABI size must be 64 bytes");

/* 模拟中断嵌套深度 */
volatile uint32_t interrupt_nest = 0;

/* 模拟任务运行上下文 */
struct mock_task_ctx {
    struct task_struct   task;
    pthread_mutex_t      mtx;
    pthread_cond_t       cv;
    bool                 woken;
    bool                 in_wait;
    int                  call_result;
    int                  recv_result;
    int                  reply_result;
    struct nos_endpoint *target_ep;
    uint32_t             timeout;
    uint32_t             badge;
    struct nos_ipc_msg   send_msg;
    struct nos_ipc_msg   resp_msg;
    struct nos_ipc_msg   recv_msg;
    struct task_struct  *caller;
};

static _Thread_local struct task_struct *s_current_task = NULL;

struct task_struct *task_current(void)
{
    return s_current_task;
}

/* 模拟单调时钟 */
static uint64_t s_mock_ticks = 5000ULL;

uint64_t cpu_run_ticks(void)
{
    return s_mock_ticks;
}

int task_hang_lock(struct task_struct *task)
{
    if (task != NULL) {
        task->status = 0x20; /* TASK_BLOCKED */
    }
    return 0;
}

void task_set_wchan(struct task_struct *task, const char *name, uintptr_t addr)
{
    if (task != NULL) {
        task->wchan_name = name;
        task->wchan_addr = addr;
    }
}

void task_clear_wchan(struct task_struct *task)
{
    if (task != NULL) {
        task->wchan_name = NULL;
        task->wchan_addr = 0;
    }
}

int task_resume(struct task_struct *task)
{
    (void)task;
    return 0;
}

int pr_log(bool no_tag, enum log_level level, const char *fmt, ...)
{
    (void)no_tag;
    (void)level;
    (void)fmt;
    return 0;
}

int timer_start(struct timer *timer, u32 tick)
{
    if (timer != NULL) {
        timer->init_tick = tick;
        timer->flags |= TIMER_FLAG_RUNNING;
    }
    return 0;
}

int timer_stop(struct timer *timer)
{
    if (timer != NULL) {
        timer->flags &= ~TIMER_FLAG_RUNNING;
    }
    return 0;
}

void *kmalloc(size_t size, gfp_t flags)
{
    (void)flags;
    return malloc(size);
}

void kfree(void *ptr)
{
    free(ptr);
}

void nos_task_wake_up_locked(struct task_struct *task)
{
    if (task == NULL) {
        return;
    }
    task->status = 0x02; /* TASK_READY */
    struct mock_task_ctx *ctx = (struct mock_task_ctx *)task;
    pthread_mutex_lock(&ctx->mtx);
    ctx->woken = true;
    pthread_cond_broadcast(&ctx->cv);
    pthread_mutex_unlock(&ctx->mtx);
}

void switch_task(void)
{
    struct task_struct *task = s_current_task;
    if (task == NULL) {
        return;
    }
    struct mock_task_ctx *ctx = (struct mock_task_ctx *)task;
    pthread_mutex_lock(&ctx->mtx);
    ctx->in_wait = true;
    pthread_cond_broadcast(&ctx->cv);
    while (!ctx->woken) {
        pthread_cond_wait(&ctx->cv, &ctx->mtx);
    }
    ctx->in_wait = false;
    pthread_mutex_unlock(&ctx->mtx);
}

int task_pi_boost(struct task_struct *owner, uint8_t waiter_prio)
{
    if (owner == NULL) {
        return -EINVAL;
    }
    spin_lock_irq(&owner->lock);
    if (waiter_prio < owner->current_priority) {
        owner->current_priority = waiter_prio;
    }
    spin_unlock_irq(&owner->lock);
    return 0;
}

int task_pi_unboost(struct task_struct *owner, uint8_t next_waiter_prio)
{
    (void)next_waiter_prio;
    if (owner == NULL) {
        return -EINVAL;
    }
    spin_lock_irq(&owner->lock);
    owner->current_priority = owner->init_priority;
    spin_unlock_irq(&owner->lock);
    return 0;
}

static void init_mock_ctx(struct mock_task_ctx *ctx, const char *name, uint8_t prio, pid_t pid)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->task.name = name;
    ctx->task.pid = pid;
    ctx->task.current_priority = prio;
    ctx->task.init_priority = prio;
    ctx->task.status = 0x02; /* TASK_READY */
    spin_lock_init(&ctx->task.lock);
    nos_refcount_init(&ctx->task.refcount, 1U);
    nos_wait_entry_init(&ctx->task.wait_entry, &ctx->task);
    nos_cap_table_init(&ctx->task.cap_table);
    pthread_mutex_init(&ctx->mtx, NULL);
    pthread_cond_init(&ctx->cv, NULL);
    ctx->woken = false;
    ctx->in_wait = false;
}

static void destroy_mock_ctx(struct mock_task_ctx *ctx)
{
    nos_cap_table_destroy(&ctx->task.cap_table);
    pthread_cond_destroy(&ctx->cv);
    pthread_mutex_destroy(&ctx->mtx);
}

static void wait_for_task_in_wait(struct mock_task_ctx *ctx)
{
    pthread_mutex_lock(&ctx->mtx);
    while (!ctx->in_wait) {
        pthread_cond_wait(&ctx->cv, &ctx->mtx);
    }
    pthread_mutex_unlock(&ctx->mtx);
}

static void *client_call_worker(void *arg)
{
    struct mock_task_ctx *ctx = (struct mock_task_ctx *)arg;
    s_current_task = &ctx->task;
    ctx->call_result = nos_endpoint_call(ctx->target_ep,
                                         ctx->badge,
                                         &ctx->send_msg,
                                         &ctx->resp_msg,
                                         ctx->timeout);
    return NULL;
}

static void *server_recv_worker(void *arg)
{
    struct mock_task_ctx *ctx = (struct mock_task_ctx *)arg;
    s_current_task = &ctx->task;
    ctx->recv_result = nos_endpoint_recv(ctx->target_ep,
                                         &ctx->recv_msg,
                                         &ctx->caller,
                                         ctx->timeout);
    return NULL;
}

/**
 * 测试 1: 正常 Client-Server 同步 RPC 调用与优先级继承
 */
static void test_ipc_sync_rpc_and_priority_inheritance(void)
{
    struct nos_endpoint ep;
    struct mock_task_ctx client;
    struct mock_task_ctx server;
    pthread_t client_tid, server_tid;
    int rc;

    rc = nos_endpoint_init(&ep, 101U);
    assert(rc == 0);

    /* 客户端优先级高 (prio=5)，服务端优先级低 (prio=20) */
    init_mock_ctx(&client, "client_hi", 5, 1001);
    init_mock_ctx(&server, "server_lo", 20, 2001);

    client.target_ep = &ep;
    client.badge = 0xABCD;
    client.timeout = NOS_IPC_TIMEOUT_INFINITE;
    memset(&client.send_msg, 0, sizeof(client.send_msg));
    client.send_msg.tag = NOS_IPC_TAG_MAKE(0x10, 0, 0);
    client.send_msg.regs[0] = 0x11223344;
    client.send_msg.regs[1] = 0x55667788;

    server.target_ep = &ep;
    server.timeout = NOS_IPC_TIMEOUT_INFINITE;

    /* 服务端启动并阻塞在 nos_endpoint_recv */
    pthread_create(&server_tid, NULL, server_recv_worker, &server);
    wait_for_task_in_wait(&server);

    /* 客户端发起 nos_endpoint_call，交付请求并进入 reply_queue 等待回复 */
    pthread_create(&client_tid, NULL, client_call_worker, &client);

    /* 等待服务端接收完成 */
    pthread_join(server_tid, NULL);
    assert(server.recv_result == 0);
    assert(server.recv_msg.regs[0] == 0x11223344);
    assert(server.recv_msg.regs[1] == 0x55667788);
    assert(server.recv_msg.token == 0xABCD);
    assert(server.caller == &client.task);

    /* 验证优先级继承：服务端被提升至高优先级 (5) */
    assert(server.task.current_priority == 5);

    /* 此时客户端已在 reply_queue 中等待回复 */
    wait_for_task_in_wait(&client);

    /* 服务端回复客户端 */
    s_current_task = &server.task;
    struct nos_ipc_msg reply;
    memset(&reply, 0, sizeof(reply));
    reply.tag = NOS_IPC_TAG_MAKE(0x10, 0, 0);
    reply.status = 0;
    reply.regs[0] = 0xAABBCCDD;
    reply.regs[1] = 0x99887766;

    rc = nos_endpoint_reply_wait(&ep, server.caller, &reply, NULL, NULL, NOS_IPC_TIMEOUT_NONBLOCK);
    assert(rc == 0);

    /* 验证服务端优先级恢复至原优先级 (20) */
    assert(server.task.current_priority == 20);

    /* 客户端完成唤醒并退出 */
    pthread_join(client_tid, NULL);
    assert(client.call_result == 0);
    assert(client.resp_msg.regs[0] == 0xAABBCCDD);
    assert(client.resp_msg.regs[1] == 0x99887766);

    nos_endpoint_close(&ep);
    destroy_mock_ctx(&client);
    destroy_mock_ctx(&server);
    printf("[PASS] test_ipc_sync_rpc_and_priority_inheritance\n");
}

/**
 * 测试 2: 客户端超时时，服务端回复安全返回 -ETIMEDOUT，不踩踏已失效内存 (UAF 防护)
 */
static void test_ipc_client_timeout_and_safe_reply(void)
{
    struct nos_endpoint ep;
    struct mock_task_ctx client;
    struct mock_task_ctx server;
    pthread_t client_tid, server_tid;
    int rc;

    rc = nos_endpoint_init(&ep, 102U);
    assert(rc == 0);

    init_mock_ctx(&client, "client_timeout", 10, 1002);
    init_mock_ctx(&server, "server_worker", 15, 2002);

    client.target_ep = &ep;
    client.badge = 0x5555;
    client.timeout = 50; /* 50 ticks 超时 */
    memset(&client.send_msg, 0, sizeof(client.send_msg));
    client.send_msg.tag = NOS_IPC_TAG_MAKE(0x20, 0, 0);

    server.target_ep = &ep;
    server.timeout = NOS_IPC_TIMEOUT_INFINITE;

    /* 服务端先等待接收 */
    pthread_create(&server_tid, NULL, server_recv_worker, &server);
    wait_for_task_in_wait(&server);

    /* 客户端发起调用，请求已交付给服务端，客户端进入 reply_queue */
    pthread_create(&client_tid, NULL, client_call_worker, &client);
    pthread_join(server_tid, NULL);
    assert(server.recv_result == 0);
    assert(server.caller == &client.task);

    wait_for_task_in_wait(&client);

    /* 模拟客户端定时器超时唤醒 */
    nos_task_wake_up_locked(&client.task);

    /* 客户端因超时返回 -ETIMEDOUT 并退出 */
    pthread_join(client_tid, NULL);
    assert(client.call_result == -ETIMEDOUT);

    /* 客户端退出后，在其原内存区域填入金丝雀数据 (Canary) */
    memset(&client.task.ipc_msg, 0xEE, sizeof(client.task.ipc_msg));

    /* 服务端随后尝试回复已经超时的客户端 */
    s_current_task = &server.task;
    struct nos_ipc_msg stale_reply;
    memset(&stale_reply, 0, sizeof(stale_reply));
    stale_reply.regs[0] = 0xDEADBEEF;

    rc = nos_endpoint_reply_wait(&ep, server.caller, &stale_reply, NULL, NULL, NOS_IPC_TIMEOUT_NONBLOCK);
    /* 必须安全返回 -ETIMEDOUT，严禁踩踏 client 内存 */
    assert(rc == -ETIMEDOUT);

    /* 验证客户端内存中的金丝雀未被服务端覆写 */
    uint8_t *canary = (uint8_t *)&client.task.ipc_msg;
    for (size_t i = 0; i < sizeof(client.task.ipc_msg); i++) {
        assert(canary[i] == 0xEE);
    }

    nos_endpoint_close(&ep);
    destroy_mock_ctx(&client);
    destroy_mock_ctx(&server);
    printf("[PASS] test_ipc_client_timeout_and_safe_reply\n");
}

/**
 * 测试 3: Endpoint 关闭时，reply_queue、send_queue 和 recv_queue 均被安全唤醒返回 -EPIPE
 */
static void test_ipc_endpoint_close_drains_all_queues(void)
{
    struct nos_endpoint ep;
    struct mock_task_ctx client_reply;
    struct mock_task_ctx client_send;
    struct mock_task_ctx server_recv1;
    struct mock_task_ctx server_recv2;
    pthread_t t_creply, t_csend, t_srecv1, t_srecv2;
    int rc;

    rc = nos_endpoint_init(&ep, 103U);
    assert(rc == 0);

    init_mock_ctx(&client_reply, "client_reply", 10, 1003);
    init_mock_ctx(&client_send,  "client_send",  11, 1004);
    init_mock_ctx(&server_recv1, "server_recv1", 12, 2003);
    init_mock_ctx(&server_recv2, "server_recv2", 13, 2004);

    client_reply.target_ep = &ep;
    client_reply.timeout = NOS_IPC_TIMEOUT_INFINITE;

    client_send.target_ep = &ep;
    client_send.timeout = NOS_IPC_TIMEOUT_INFINITE;

    server_recv1.target_ep = &ep;
    server_recv1.timeout = NOS_IPC_TIMEOUT_INFINITE;

    server_recv2.target_ep = &ep;
    server_recv2.timeout = NOS_IPC_TIMEOUT_INFINITE;

    /* 1. server1 接收并使 client_reply 进入 reply_queue */
    pthread_create(&t_srecv1, NULL, server_recv_worker, &server_recv1);
    wait_for_task_in_wait(&server_recv1);

    pthread_create(&t_creply, NULL, client_call_worker, &client_reply);
    pthread_join(t_srecv1, NULL);
    assert(server_recv1.recv_result == 0);
    wait_for_task_in_wait(&client_reply);

    /* 2. client_send 挂入 send_queue */
    pthread_create(&t_csend, NULL, client_call_worker, &client_send);
    wait_for_task_in_wait(&client_send);

    /* 3. server_recv2 挂入 recv_queue */
    /* 注意：此时 client_send 在 send_queue 中，如果 server_recv2 启动会立刻接走 client_send。
     * 因此为让 recv_queue 也有等待者，先让 server_recv2 接走 client_send，再让另一个 server 挂入 recv_queue */
    pthread_create(&t_srecv2, NULL, server_recv_worker, &server_recv2);
    pthread_join(t_srecv2, NULL);
    assert(server_recv2.recv_result == 0);
    wait_for_task_in_wait(&client_send); /* client_send 现在也在 reply_queue 中了 */

    /* 再建一个 server 挂入空的 recv_queue */
    struct mock_task_ctx server_recv3;
    pthread_t t_srecv3;
    init_mock_ctx(&server_recv3, "server_recv3", 14, 2005);
    server_recv3.target_ep = &ep;
    server_recv3.timeout = NOS_IPC_TIMEOUT_INFINITE;
    pthread_create(&t_srecv3, NULL, server_recv_worker, &server_recv3);
    wait_for_task_in_wait(&server_recv3);

    /* 此时：client_reply 和 client_send 均在 reply_queue 中，server_recv3 在 recv_queue 中 */
    /* 执行 nos_endpoint_close，必须安全唤醒所有队列 */
    nos_endpoint_close(&ep);

    pthread_join(t_creply, NULL);
    pthread_join(t_csend, NULL);
    pthread_join(t_srecv3, NULL);

    assert(client_reply.call_result == -EPIPE);
    assert(client_send.call_result == -EPIPE);
    assert(server_recv3.recv_result == -EPIPE);

    destroy_mock_ctx(&client_reply);
    destroy_mock_ctx(&client_send);
    destroy_mock_ctx(&server_recv1);
    destroy_mock_ctx(&server_recv2);
    destroy_mock_ctx(&server_recv3);
    printf("[PASS] test_ipc_endpoint_close_drains_all_queues\n");
}

/**
 * 测试 4: Capability 查找递增引用与并发安全销毁 (Pinned Lookup & Concurrent Destroy)
 */
static void test_ipc_capability_pinning_and_concurrent_destroy(void)
{
    struct mock_task_ctx task_ctx;
    init_mock_ctx(&task_ctx, "task_cap", 10, 3001);
    s_current_task = &task_ctx.task;

    nos_handle_t ep_cap = NOS_HANDLE_INVALID;
    struct nos_endpoint *ep = nos_endpoint_create(&ep_cap);
    assert(ep != NULL);
    assert(ep_cap != NOS_HANDLE_INVALID);

    /* 初始时：ep 拥有创建者持有的 1 个引用 */
    assert(nos_refcount_read(&ep->object.refcount) == 1);

    /* 通过 nos_cap_lookup 查找 endpoint 对象，应自动增引用固定 (Pinning) */
    struct nos_endpoint *looked_ep = NULL;
    uint32_t badge = 0;
    int rc = nos_cap_lookup(&task_ctx.task.cap_table,
                            ep_cap,
                            NOS_CAP_ENDPOINT,
                            NOS_CAP_RIGHT_SEND,
                            (void **)&looked_ep,
                            &badge);
    assert(rc == 0);
    assert(looked_ep == ep);
    /* 引用计数必须递增为 2 */
    assert(nos_refcount_read(&ep->object.refcount) == 2);

    /* 并发路径执行销毁：nos_endpoint_destroy 关闭并退休该对象 */
    nos_endpoint_destroy(ep);

    /* 此时对象已关闭且 retired，但由于 lookup 仍持有 1 个在途引用，内存未释放 */
    assert(ep->object.closed == true);
    assert(ep->object.retired == true);
    assert(nos_refcount_read(&ep->object.refcount) == 1);

    /* 此时再次通过 cap lookup 应该返回错误 (对象已退休/正在析构) */
    struct nos_endpoint *second_lookup = NULL;
    rc = nos_cap_lookup(&task_ctx.task.cap_table,
                        ep_cap,
                        NOS_CAP_ENDPOINT,
                        NOS_CAP_RIGHT_SEND,
                        (void **)&second_lookup,
                        NULL);
    /* 已 retire 或处于销毁中的对象无法再次 get 引用 */
    assert(rc == -ESTALE);

    /* 在途系统调用完成操作，释放先前固定的引用 */
    bool freed = nos_object_put(&looked_ep->object);
    assert(freed == true); /* 引用归零，释放内存 */

    destroy_mock_ctx(&task_ctx);
    printf("[PASS] test_ipc_capability_pinning_and_concurrent_destroy\n");
}

int main(void)
{
    printf("=== Running Microkernel Synchronous Endpoint IPC Tests ===\n");
    test_ipc_sync_rpc_and_priority_inheritance();
    test_ipc_client_timeout_and_safe_reply();
    test_ipc_endpoint_close_drains_all_queues();
    test_ipc_capability_pinning_and_concurrent_destroy();
    printf("All Microkernel Synchronous Endpoint IPC tests passed successfully!\n");
    return 0;
}
