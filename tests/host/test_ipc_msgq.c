/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * 宿主端测试：有界定长消息队列 (nos_msgq) 核心行为验证
 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <kernel/spinlock.h>
#include <kernel/task.h>
#include <kernel/timer.h>
#include <kernel/printk.h>
#include <nos/refcount.h>
#include <nos/object.h>
#include <nos/timeout.h>
#include <nos/wait.h>
#include <nos/msgq.h>

/* 模拟中断嵌套深度 */
volatile uint32_t interrupt_nest = 0;

/* 模拟任务运行上下文 */
struct mock_task_ctx {
    struct task_struct      task;
    pthread_mutex_t         mtx;
    pthread_cond_t          cv;
    bool                    woken;
    bool                    in_wait;
    int                     result;
    struct nos_msgq        *target_q;
    nos_timeout_t           timeout;
    uint32_t                msg_val;
    size_t                  msg_size;
};

static _Thread_local struct task_struct *s_current_task = NULL;

struct task_struct *task_current(void)
{
    return s_current_task;
}

/* 模拟单调时钟 */
static uint64_t s_mock_ticks = 1000ULL;

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

static void init_mock_ctx(struct mock_task_ctx *ctx, const char *name, uint8_t prio)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->task.name = name;
    ctx->task.current_priority = prio;
    ctx->task.init_priority = prio;
    ctx->task.status = 0x02; /* TASK_READY */
    spin_lock_init(&ctx->task.lock);
    nos_refcount_init(&ctx->task.refcount, 1U);
    nos_wait_entry_init(&ctx->task.wait_entry, &ctx->task);
    pthread_mutex_init(&ctx->mtx, NULL);
    pthread_cond_init(&ctx->cv, NULL);
    ctx->woken = false;
    ctx->in_wait = false;
    ctx->result = 0;
}

static void destroy_mock_ctx(struct mock_task_ctx *ctx)
{
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

static void *task_send_worker(void *arg)
{
    struct mock_task_ctx *ctx = (struct mock_task_ctx *)arg;
    s_current_task = &ctx->task;
    ctx->result = nos_msgq_send(ctx->target_q, &ctx->msg_val, ctx->msg_size, ctx->timeout);
    return NULL;
}

static void *task_recv_worker(void *arg)
{
    struct mock_task_ctx *ctx = (struct mock_task_ctx *)arg;
    s_current_task = &ctx->task;
    ctx->result = nos_msgq_recv(ctx->target_q, &ctx->msg_val, ctx->msg_size, ctx->timeout);
    return NULL;
}

/**
 * 测试 1: 缓冲区大小算术溢出校验与合法初始化
 */
static void test_msgq_init_and_arithmetic_overflow(void)
{
    struct nos_msgq q;
    uint8_t buf[128];

    /* 空指针参数校验 */
    assert(nos_msgq_init(NULL, buf, sizeof(buf), 4, 10) == -EINVAL);
    assert(nos_msgq_init(&q, NULL, sizeof(buf), 4, 10) == -EINVAL);

    /* 0 容量或 0 元素大小校验 */
    assert(nos_msgq_init(&q, buf, sizeof(buf), 0, 10) == -EINVAL);
    assert(nos_msgq_init(&q, buf, sizeof(buf), 4, 0) == -EINVAL);

    /* 缓冲区字节数不足容纳 capacity * element_size */
    assert(nos_msgq_init(&q, buf, 39, 4, 10) == -EINVAL);

    /* 算术溢出校验：超大 capacity 导致 capacity * element_size 环绕 */
    assert(nos_msgq_init(&q, buf, 128, 1024, (uint32_t)UINT32_MAX) == -EINVAL);

    /* 恰好匹配与富余缓冲区合法初始化 */
    assert(nos_msgq_init(&q, buf, 40, 4, 10) == 0);
    assert(q.capacity == 10U);
    assert(q.element_size == 4U);
    assert(q.head == 0U);
    assert(q.tail == 0U);
    assert(q.count == 0U);
    assert(nos_wait_queue_empty(&q.wait_send));
    assert(nos_wait_queue_empty(&q.wait_recv));
    assert(!nos_object_is_closed(&q.object));

    /* 富余缓冲区 */
    assert(nos_msgq_init(&q, buf, sizeof(buf), 4, 10) == 0);

    puts("[PASS] test_msgq_init_and_arithmetic_overflow");
}

/**
 * 测试 2: 元素大小精确匹配校验 (发送不一致返回 -EINVAL，接收过小返回 -EMSGSIZE)
 */
static void test_msgq_element_size_and_param_validation(void)
{
    struct nos_msgq q;
    uint32_t buf[8];
    uint32_t val = 0x12345678;
    uint16_t small_val = 0x1234;
    uint64_t large_val = 0;
    uint32_t recv_val = 0;

    assert(nos_msgq_init(&q, buf, sizeof(buf), sizeof(uint32_t), 4) == 0);

    /* 空指针参数校验 */
    assert(nos_msgq_send(NULL, &val, sizeof(val), NOS_NO_WAIT) == -EINVAL);
    assert(nos_msgq_send(&q, NULL, sizeof(val), NOS_NO_WAIT) == -EINVAL);
    assert(nos_msgq_recv(NULL, &recv_val, sizeof(recv_val), NOS_NO_WAIT) == -EINVAL);
    assert(nos_msgq_recv(&q, NULL, sizeof(recv_val), NOS_NO_WAIT) == -EINVAL);

    /* 发送大小不匹配校验 (必须严格精确等于 element_size) */
    assert(nos_msgq_send(&q, &small_val, sizeof(small_val), NOS_NO_WAIT) == -EINVAL);
    assert(nos_msgq_send(&q, &large_val, sizeof(large_val), NOS_NO_WAIT) == -EINVAL);

    /* 发送一条合法消息 */
    assert(nos_msgq_send(&q, &val, sizeof(val), NOS_NO_WAIT) == 0);
    assert(q.count == 1U);

    /* 接收缓冲区过小校验 (必须 >= element_size)，且不得截断或消耗消息 */
    uint16_t small_recv = 0;
    assert(nos_msgq_recv(&q, &small_recv, sizeof(small_recv), NOS_NO_WAIT) == -EMSGSIZE);
    assert(q.count == 1U);

    /* 接收缓冲区大小富余 (>= element_size) 正常接收 */
    uint64_t large_recv = 0;
    assert(nos_msgq_recv(&q, &large_recv, sizeof(large_recv), NOS_NO_WAIT) == 0);
    assert((uint32_t)(large_recv & 0xFFFFFFFFU) == val);
    assert(q.count == 0U);

    puts("[PASS] test_msgq_element_size_and_param_validation");
}

/**
 * 测试 3: 环形队列先进先出 (FIFO) 与跨界环回 (wrap-around) 数据完整性
 */
static void test_msgq_fifo_wrap_around_integrity(void)
{
    struct nos_msgq q;
    uint32_t buf[4];
    uint32_t item;

    assert(nos_msgq_init(&q, buf, sizeof(buf), sizeof(uint32_t), 4) == 0);

    /* 写入 4 条填满队列: 10, 20, 30, 40 */
    for (uint32_t i = 1; i <= 4; i++) {
        item = i * 10;
        assert(nos_msgq_send(&q, &item, sizeof(item), NOS_NO_WAIT) == 0);
    }
    assert(q.count == 4U);

    /* 读取 2 条: 应依次为 10, 20 */
    assert(nos_msgq_recv(&q, &item, sizeof(item), NOS_NO_WAIT) == 0);
    assert(item == 10);
    assert(nos_msgq_recv(&q, &item, sizeof(item), NOS_NO_WAIT) == 0);
    assert(item == 20);
    assert(q.count == 2U);
    assert(q.head == 2U);

    /* 写入 2 条发生环回 (wrap-around): 50, 60 */
    item = 50;
    assert(nos_msgq_send(&q, &item, sizeof(item), NOS_NO_WAIT) == 0);
    item = 60;
    assert(nos_msgq_send(&q, &item, sizeof(item), NOS_NO_WAIT) == 0);
    assert(q.count == 4U);
    assert(q.tail == 2U);

    /* 再次全部读出: 应严格按 FIFO 顺序为 30, 40, 50, 60 */
    uint32_t expected[] = {30, 40, 50, 60};
    for (int i = 0; i < 4; i++) {
        assert(nos_msgq_recv(&q, &item, sizeof(item), NOS_NO_WAIT) == 0);
        assert(item == expected[i]);
    }
    assert(q.count == 0U);
    assert(q.head == 2U);

    puts("[PASS] test_msgq_fifo_wrap_around_integrity");
}

/**
 * 测试 4: 队列满时阻塞发送与队列空时阻塞接收 (双向背压验证)
 */
static void test_msgq_blocking_send_recv_backpressure(void)
{
    struct nos_msgq q;
    uint32_t buf[2];
    struct mock_task_ctx receiver_task;
    struct mock_task_ctx sender_task;
    pthread_t th_recv, th_send;
    uint32_t item;

    assert(nos_msgq_init(&q, buf, sizeof(buf), sizeof(uint32_t), 2) == 0);

    /* === 场景 A: 队列空时阻塞接收，发送者生产后唤醒 === */
    init_mock_ctx(&receiver_task, "receiver_t", 10);
    receiver_task.target_q = &q;
    receiver_task.msg_size = sizeof(uint32_t);
    receiver_task.timeout = NOS_WAIT_FOREVER;

    assert(pthread_create(&th_recv, NULL, task_recv_worker, &receiver_task) == 0);
    wait_for_task_in_wait(&receiver_task);

    /* 确认接收者正在 wait_recv 队列排队 */
    assert(nos_wait_queue_count(&q.wait_recv) == 1U);
    assert(q.count == 0U);

    /* 主线程作为生产者发送一条消息 */
    item = 8888;
    assert(nos_msgq_send(&q, &item, sizeof(item), NOS_WAIT_FOREVER) == 0);

    pthread_join(th_recv, NULL);
    assert(receiver_task.result == 0);
    assert(receiver_task.msg_val == 8888);
    assert(nos_wait_queue_empty(&q.wait_recv));
    assert(q.count == 0U);
    destroy_mock_ctx(&receiver_task);

    /* === 场景 B: 队列满时阻塞发送，接收者消费后唤醒 === */
    /* 先填满容量为 2 的队列 */
    item = 1001;
    assert(nos_msgq_send(&q, &item, sizeof(item), NOS_NO_WAIT) == 0);
    item = 1002;
    assert(nos_msgq_send(&q, &item, sizeof(item), NOS_NO_WAIT) == 0);
    assert(q.count == 2U);

    init_mock_ctx(&sender_task, "sender_t", 10);
    sender_task.target_q = &q;
    sender_task.msg_val = 1003;
    sender_task.msg_size = sizeof(uint32_t);
    sender_task.timeout = NOS_WAIT_FOREVER;

    assert(pthread_create(&th_send, NULL, task_send_worker, &sender_task) == 0);
    wait_for_task_in_wait(&sender_task);

    /* 确认发送者正在 wait_send 队列排队 */
    assert(nos_wait_queue_count(&q.wait_send) == 1U);
    assert(q.count == 2U);

    /* 主线程消费一条消息腾出空间 */
    uint32_t recv_item = 0;
    assert(nos_msgq_recv(&q, &recv_item, sizeof(recv_item), NOS_WAIT_FOREVER) == 0);
    assert(recv_item == 1001);

    pthread_join(th_send, NULL);
    assert(sender_task.result == 0);
    assert(nos_wait_queue_empty(&q.wait_send));

    /* 此时队列中应有 1002 和阻塞发送者送入的 1003 */
    assert(nos_msgq_recv(&q, &recv_item, sizeof(recv_item), NOS_WAIT_FOREVER) == 0);
    assert(recv_item == 1002);
    assert(nos_msgq_recv(&q, &recv_item, sizeof(recv_item), NOS_WAIT_FOREVER) == 0);
    assert(recv_item == 1003);
    assert(q.count == 0U);

    destroy_mock_ctx(&sender_task);

    puts("[PASS] test_msgq_blocking_send_recv_backpressure");
}

/**
 * 测试 5: 超时唤醒判定 (队列空读与满写返回 -ETIMEDOUT 且状态不破坏)
 */
static void test_msgq_timeout_send_recv(void)
{
    struct nos_msgq q;
    uint32_t buf[1];
    struct mock_task_ctx t_recv;
    struct mock_task_ctx t_send;
    pthread_t th;
    uint32_t item = 123;

    assert(nos_msgq_init(&q, buf, sizeof(buf), sizeof(uint32_t), 1) == 0);

    /* 1. 队列空接收超时 */
    init_mock_ctx(&t_recv, "timeout_recv", 10);
    t_recv.target_q = &q;
    t_recv.msg_size = sizeof(uint32_t);
    t_recv.timeout = 50ULL;

    assert(pthread_create(&th, NULL, task_recv_worker, &t_recv) == 0);
    wait_for_task_in_wait(&t_recv);
    assert(nos_wait_queue_count(&q.wait_recv) == 1U);

    /* 模拟定时器超时唤醒 */
    pthread_mutex_lock(&t_recv.mtx);
    t_recv.woken = true;
    pthread_cond_broadcast(&t_recv.cv);
    pthread_mutex_unlock(&t_recv.mtx);

    pthread_join(th, NULL);
    assert(t_recv.result == -ETIMEDOUT);
    assert(nos_wait_queue_empty(&q.wait_recv));
    destroy_mock_ctx(&t_recv);

    /* 2. 队列满发送超时 */
    assert(nos_msgq_send(&q, &item, sizeof(item), NOS_NO_WAIT) == 0);
    assert(q.count == 1U);

    init_mock_ctx(&t_send, "timeout_send", 10);
    t_send.target_q = &q;
    t_send.msg_val = 456;
    t_send.msg_size = sizeof(uint32_t);
    t_send.timeout = 50ULL;

    assert(pthread_create(&th, NULL, task_send_worker, &t_send) == 0);
    wait_for_task_in_wait(&t_send);
    assert(nos_wait_queue_count(&q.wait_send) == 1U);

    /* 模拟定时器超时唤醒 */
    pthread_mutex_lock(&t_send.mtx);
    t_send.woken = true;
    pthread_cond_broadcast(&t_send.cv);
    pthread_mutex_unlock(&t_send.mtx);

    pthread_join(th, NULL);
    assert(t_send.result == -ETIMEDOUT);
    assert(nos_wait_queue_empty(&q.wait_send));
    destroy_mock_ctx(&t_send);

    /* 确认队列中原消息 123 未被篡改且可读出 */
    uint32_t recv_item = 0;
    assert(nos_msgq_recv(&q, &recv_item, sizeof(recv_item), NOS_NO_WAIT) == 0);
    assert(recv_item == 123);
    assert(q.count == 0U);

    puts("[PASS] test_msgq_timeout_send_recv");
}

/**
 * 测试 6: 非阻塞与 ISR 快速失败 (队列满发返回 -ENOSPC / -EAGAIN，队列空读返回 -EAGAIN)
 */
static void test_msgq_nonblocking_and_isr_fast_fail(void)
{
    struct nos_msgq q;
    uint32_t buf[2];
    uint32_t val = 999;
    uint32_t recv_val = 0;

    assert(nos_msgq_init(&q, buf, sizeof(buf), sizeof(uint32_t), 2) == 0);

    /* 1. 队列为空时非阻塞读返回 -EAGAIN */
    assert(nos_msgq_recv(&q, &recv_val, sizeof(recv_val), NOS_NO_WAIT) == -EAGAIN);

    /* 2. 在 ISR 模式下 (interrupt_nest > 0) 队列为空时读返回 -EAGAIN */
    interrupt_nest = 1;
    assert(nos_msgq_recv(&q, &recv_val, sizeof(recv_val), NOS_NO_WAIT) == -EAGAIN);
    assert(nos_msgq_recv(&q, &recv_val, sizeof(recv_val), NOS_WAIT_FOREVER) == -EAGAIN);

    /* 3. 在 ISR 模式下队列有空间时正常发送并成功 */
    val = 101;
    assert(nos_msgq_send(&q, &val, sizeof(val), NOS_NO_WAIT) == 0);
    val = 102;
    assert(nos_msgq_send(&q, &val, sizeof(val), NOS_NO_WAIT) == 0);
    assert(q.count == 2U);

    /* 4. 队列满时在 ISR 模式下快速失败返回 -ENOSPC */
    val = 103;
    assert(nos_msgq_send(&q, &val, sizeof(val), NOS_NO_WAIT) == -ENOSPC);
    assert(nos_msgq_send(&q, &val, sizeof(val), NOS_WAIT_FOREVER) == -ENOSPC);

    /* 退出 ISR 模式 */
    interrupt_nest = 0;

    /* 5. 退出 ISR 模式后，普通任务上下文非阻塞满发返回 -EAGAIN */
    assert(nos_msgq_send(&q, &val, sizeof(val), NOS_NO_WAIT) == -EAGAIN);

    /* 6. 正常消费一条消息后，ISR 发送恢复成功 */
    assert(nos_msgq_recv(&q, &recv_val, sizeof(recv_val), NOS_NO_WAIT) == 0);
    assert(recv_val == 101);
    assert(q.count == 1U);

    interrupt_nest = 1;
    val = 202;
    assert(nos_msgq_send(&q, &val, sizeof(val), NOS_NO_WAIT) == 0);
    assert(q.count == 2U);
    interrupt_nest = 0;

    puts("[PASS] test_msgq_nonblocking_and_isr_fast_fail");
}

/**
 * 测试 7: MPMC 多任务并发读写与优先级唤醒验证
 */
static void test_msgq_mpmc_concurrent(void)
{
    struct nos_msgq q;
    uint32_t buf[1]; /* 容量为 1，确保每个发送均产生背压 */
    struct mock_task_ctx prod_hi, prod_lo;
    struct mock_task_ctx cons_hi, cons_lo;
    pthread_t th_phi, th_plo, th_chi, th_clo;
    uint32_t item;

    assert(nos_msgq_init(&q, buf, sizeof(buf), sizeof(uint32_t), 1) == 0);

    /*
     * 场景 A: 接收端多任务优先级排队与唤醒 (MPMC Consumer Priority)
     * 队列为空，低优先级消费者 cons_lo (prio 20) 先排队，高优先级 cons_hi (prio 10) 后排队
     */
    init_mock_ctx(&cons_lo, "cons_lo", 20);
    init_mock_ctx(&cons_hi, "cons_hi", 10);
    cons_lo.target_q = &q;
    cons_lo.msg_size = sizeof(uint32_t);
    cons_lo.timeout = NOS_WAIT_FOREVER;
    cons_hi.target_q = &q;
    cons_hi.msg_size = sizeof(uint32_t);
    cons_hi.timeout = NOS_WAIT_FOREVER;

    assert(pthread_create(&th_clo, NULL, task_recv_worker, &cons_lo) == 0);
    wait_for_task_in_wait(&cons_lo);
    assert(pthread_create(&th_chi, NULL, task_recv_worker, &cons_hi) == 0);
    wait_for_task_in_wait(&cons_hi);

    assert(nos_wait_queue_count(&q.wait_recv) == 2U);

    /* 发送一条消息，必须优先唤醒高优先级消费者 cons_hi */
    item = 5555;
    assert(nos_msgq_send(&q, &item, sizeof(item), NOS_WAIT_FOREVER) == 0);

    pthread_join(th_chi, NULL);
    assert(cons_hi.result == 0);
    assert(cons_hi.msg_val == 5555);

    /* 此时 cons_lo 仍在等待 */
    assert(nos_wait_queue_count(&q.wait_recv) == 1U);

    /* 再次发送一条消息，唤醒 cons_lo */
    item = 6666;
    assert(nos_msgq_send(&q, &item, sizeof(item), NOS_WAIT_FOREVER) == 0);

    pthread_join(th_clo, NULL);
    assert(cons_lo.result == 0);
    assert(cons_lo.msg_val == 6666);
    assert(nos_wait_queue_empty(&q.wait_recv));

    destroy_mock_ctx(&cons_lo);
    destroy_mock_ctx(&cons_hi);

    /*
     * 场景 B: 发送端多任务优先级排队与唤醒 (MPMC Producer Priority)
     * 队列先填满，低优先级生产任务 prod_lo (prio 20) 先排队，高优先级 prod_hi (prio 10) 后排队
     */
    item = 1111;
    assert(nos_msgq_send(&q, &item, sizeof(item), NOS_NO_WAIT) == 0);
    assert(q.count == 1U);

    init_mock_ctx(&prod_lo, "prod_lo", 20);
    init_mock_ctx(&prod_hi, "prod_hi", 10);
    prod_lo.target_q = &q;
    prod_lo.msg_val = 2222;
    prod_lo.msg_size = sizeof(uint32_t);
    prod_lo.timeout = NOS_WAIT_FOREVER;

    prod_hi.target_q = &q;
    prod_hi.msg_val = 3333;
    prod_hi.msg_size = sizeof(uint32_t);
    prod_hi.timeout = NOS_WAIT_FOREVER;

    assert(pthread_create(&th_plo, NULL, task_send_worker, &prod_lo) == 0);
    wait_for_task_in_wait(&prod_lo);
    assert(pthread_create(&th_phi, NULL, task_send_worker, &prod_hi) == 0);
    wait_for_task_in_wait(&prod_hi);

    assert(nos_wait_queue_count(&q.wait_send) == 2U);

    /* 消费一条消息腾出空间，必须优先唤醒高优先级发送者 prod_hi 送入 3333 */
    uint32_t recv_val = 0;
    assert(nos_msgq_recv(&q, &recv_val, sizeof(recv_val), NOS_WAIT_FOREVER) == 0);
    assert(recv_val == 1111);

    pthread_join(th_phi, NULL);
    assert(prod_hi.result == 0);
    assert(nos_wait_queue_count(&q.wait_send) == 1U);

    /* 再次消费一条消息 (此时读出的是 prod_hi 刚写入的 3333)，腾出空间唤醒 prod_lo 送入 2222 */
    assert(nos_msgq_recv(&q, &recv_val, sizeof(recv_val), NOS_WAIT_FOREVER) == 0);
    assert(recv_val == 3333);

    pthread_join(th_plo, NULL);
    assert(prod_lo.result == 0);
    assert(nos_wait_queue_empty(&q.wait_send));

    /* 消费 prod_lo 写入的 2222，队列最终变空 */
    assert(nos_msgq_recv(&q, &recv_val, sizeof(recv_val), NOS_WAIT_FOREVER) == 0);
    assert(recv_val == 2222);
    assert(q.count == 0U);

    destroy_mock_ctx(&prod_lo);
    destroy_mock_ctx(&prod_hi);

    puts("[PASS] test_msgq_mpmc_concurrent");
}

/**
 * 测试 8: close 退出与等待任务唤醒 (返回 -EPIPE 且排干剩余消息后拒绝新操作)
 */
static void test_msgq_close_drains_and_rejects(void)
{
    struct nos_msgq q;
    uint32_t buf[2];
    struct mock_task_ctx t_recv1, t_recv2;
    struct mock_task_ctx t_send1, t_send2;
    pthread_t th_r1, th_r2, th_s1, th_s2;
    uint32_t item;

    /* 1. 测试阻塞在空队列上的接收者被 close 唤醒 */
    assert(nos_msgq_init(&q, buf, sizeof(buf), sizeof(uint32_t), 2) == 0);

    init_mock_ctx(&t_recv1, "r1", 10);
    init_mock_ctx(&t_recv2, "r2", 20);
    t_recv1.target_q = &q;
    t_recv1.msg_size = sizeof(uint32_t);
    t_recv1.timeout = NOS_WAIT_FOREVER;
    t_recv2.target_q = &q;
    t_recv2.msg_size = sizeof(uint32_t);
    t_recv2.timeout = NOS_WAIT_FOREVER;

    assert(pthread_create(&th_r1, NULL, task_recv_worker, &t_recv1) == 0);
    assert(pthread_create(&th_r2, NULL, task_recv_worker, &t_recv2) == 0);
    wait_for_task_in_wait(&t_recv1);
    wait_for_task_in_wait(&t_recv2);
    assert(nos_wait_queue_count(&q.wait_recv) == 2U);

    /* 关闭队列 */
    assert(nos_msgq_close(&q) == 0);
    assert(nos_object_is_closed(&q.object));

    pthread_join(th_r1, NULL);
    pthread_join(th_r2, NULL);
    assert(t_recv1.result == -EPIPE);
    assert(t_recv2.result == -EPIPE);
    assert(nos_wait_queue_empty(&q.wait_recv));

    destroy_mock_ctx(&t_recv1);
    destroy_mock_ctx(&t_recv2);

    /* 关闭后已为空，继续读写均立即返回 -EPIPE */
    item = 111;
    assert(nos_msgq_send(&q, &item, sizeof(item), NOS_NO_WAIT) == -EPIPE);
    assert(nos_msgq_send(&q, &item, sizeof(item), NOS_WAIT_FOREVER) == -EPIPE);
    assert(nos_msgq_recv(&q, &item, sizeof(item), NOS_NO_WAIT) == -EPIPE);
    assert(nos_msgq_recv(&q, &item, sizeof(item), NOS_WAIT_FOREVER) == -EPIPE);

    /* 幂等性：重复 close 返回 0 */
    assert(nos_msgq_close(&q) == 0);

    /* 2. 测试队列中有未读消息时 close：允许排干 (drain) 剩余消息，空后返回 -EPIPE */
    assert(nos_msgq_init(&q, buf, sizeof(buf), sizeof(uint32_t), 2) == 0);
    item = 777;
    assert(nos_msgq_send(&q, &item, sizeof(item), NOS_NO_WAIT) == 0);
    item = 888;
    assert(nos_msgq_send(&q, &item, sizeof(item), NOS_NO_WAIT) == 0);
    assert(q.count == 2U);

    /* 队列满时挂起两个发送者 */
    init_mock_ctx(&t_send1, "s1", 10);
    init_mock_ctx(&t_send2, "s2", 20);
    t_send1.target_q = &q;
    t_send1.msg_val = 991;
    t_send1.msg_size = sizeof(uint32_t);
    t_send1.timeout = NOS_WAIT_FOREVER;
    t_send2.target_q = &q;
    t_send2.msg_val = 992;
    t_send2.msg_size = sizeof(uint32_t);
    t_send2.timeout = NOS_WAIT_FOREVER;

    assert(pthread_create(&th_s1, NULL, task_send_worker, &t_send1) == 0);
    assert(pthread_create(&th_s2, NULL, task_send_worker, &t_send2) == 0);
    wait_for_task_in_wait(&t_send1);
    wait_for_task_in_wait(&t_send2);
    assert(nos_wait_queue_count(&q.wait_send) == 2U);

    /* 关闭队列：发送者全部被唤醒并返回 -EPIPE */
    assert(nos_msgq_close(&q) == 0);
    pthread_join(th_s1, NULL);
    pthread_join(th_s2, NULL);
    assert(t_send1.result == -EPIPE);
    assert(t_send2.result == -EPIPE);
    assert(nos_wait_queue_empty(&q.wait_send));

    destroy_mock_ctx(&t_send1);
    destroy_mock_ctx(&t_send2);

    /* 读取剩余的 2 条消息 */
    uint32_t drained = 0;
    assert(nos_msgq_recv(&q, &drained, sizeof(drained), NOS_NO_WAIT) == 0);
    assert(drained == 777);
    assert(nos_msgq_recv(&q, &drained, sizeof(drained), NOS_NO_WAIT) == 0);
    assert(drained == 888);
    assert(q.count == 0U);

    /* 消息排干后，再次接收返回 -EPIPE */
    assert(nos_msgq_recv(&q, &drained, sizeof(drained), NOS_NO_WAIT) == -EPIPE);

    puts("[PASS] test_msgq_close_drains_and_rejects");
}

int main(void)
{
    puts("=== Running IPC Message Queue (nos_msgq) Tests ===");

    test_msgq_init_and_arithmetic_overflow();
    test_msgq_element_size_and_param_validation();
    test_msgq_fifo_wrap_around_integrity();
    test_msgq_blocking_send_recv_backpressure();
    test_msgq_timeout_send_recv();
    test_msgq_nonblocking_and_isr_fast_fail();
    test_msgq_mpmc_concurrent();
    test_msgq_close_drains_and_rejects();

    puts("All IPC message queue tests PASS");
    return 0;
}
