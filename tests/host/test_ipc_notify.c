/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * 宿主端测试：64 位异步位图通知 (nos_notify) 核心行为验证
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

#include <kernel/spinlock.h>
#include <kernel/task.h>
#include <kernel/timer.h>
#include <kernel/printk.h>
#include <nos/refcount.h>
#include <nos/object.h>
#include <nos/timeout.h>
#include <nos/wait.h>
#include <nos/notification.h>

/* 模拟中断嵌套深度 */
volatile uint32_t interrupt_nest = 0;

/* 模拟任务运行上下文 */
struct mock_task_ctx {
    struct task_struct        task;
    pthread_mutex_t           mtx;
    pthread_cond_t            cv;
    bool                      woken;
    bool                      in_wait;
    int                       wait_result;
    struct nos_notification  *target_notif;
    nos_timeout_t             timeout;
    uint64_t                  out_bits;
};

static _Thread_local struct task_struct *s_current_task = NULL;

struct task_struct *task_current(void)
{
    return s_current_task;
}

/* 模拟单调时钟 */
static uint64_t s_mock_ticks = 2000ULL;

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

int nos_cap_alloc(struct nos_cap_table *table,
                  enum nos_cap_type type,
                  void *obj,
                  nos_cap_rights_t rights,
                  uint32_t badge,
                  nos_handle_t *out_cap)
{
    (void)table;
    (void)type;
    (void)obj;
    (void)rights;
    (void)badge;
    if (out_cap != NULL) {
        *out_cap = 1;
    }
    return 0;
}

int nos_cap_lookup(struct nos_cap_table *table,
                   nos_handle_t cap,
                   enum nos_cap_type expected_type,
                   nos_cap_rights_t required_rights,
                   void **out_obj,
                   uint32_t *out_badge)
{
    (void)table;
    (void)cap;
    (void)expected_type;
    (void)required_rights;
    (void)out_obj;
    (void)out_badge;
    return -ENOSYS;
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
    ctx->task.notify_bits = 0ULL;
    spin_lock_init(&ctx->task.lock);
    nos_refcount_init(&ctx->task.refcount, 1U);
    nos_wait_entry_init(&ctx->task.wait_entry, &ctx->task);
    pthread_mutex_init(&ctx->mtx, NULL);
    pthread_cond_init(&ctx->cv, NULL);
    ctx->woken = false;
    ctx->in_wait = false;
    ctx->wait_result = 0;
    ctx->out_bits = 0ULL;
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

static void *task_wait_worker(void *arg)
{
    struct mock_task_ctx *ctx = (struct mock_task_ctx *)arg;
    s_current_task = &ctx->task;
    ctx->wait_result = nos_notify_wait_obj(ctx->target_notif, &ctx->out_bits, ctx->timeout);
    return NULL;
}

/**
 * 测试 1: 参数边界校验与合法初始化状态验证
 */
static void test_notify_init_and_param_boundary(void)
{
    struct nos_notification notif;
    uint64_t bits = 0ULL;

    /* 空指针参数校验 */
    assert(nos_notification_init(NULL) == -EINVAL);
    assert(nos_notify_signal_obj(NULL, 1ULL) == -EINVAL);
    assert(nos_notify_wait_obj(NULL, &bits, NOS_NO_WAIT) == -EINVAL);
    assert(nos_notification_close(NULL) == -EINVAL);

    /* 成功初始化验证 */
    assert(nos_notification_init(&notif) == 0);
    assert(notif.pending_bits == 0ULL);
    assert(nos_wait_queue_empty(&notif.wait_queue) == true);
    assert(notif.object.closed == false);

    /* 0 位掩码校验 */
    assert(nos_notify_signal_obj(&notif, 0ULL) == -EINVAL);

    /* 输出位图指针为空校验 */
    assert(nos_notify_wait_obj(&notif, NULL, NOS_NO_WAIT) == -EINVAL);

    puts("[PASS] test_notify_init_and_param_boundary");
}

/**
 * 测试 2: 按位或累加、同位事件合并与非阻塞原子读清 (Read-and-Clear)
 */
static void test_notify_coalescing_and_read_clear(void)
{
    struct nos_notification notif;
    uint64_t bits = 0ULL;

    assert(nos_notification_init(&notif) == 0);

    /* 1. 队列为空时非阻塞等待返回 -EAGAIN */
    assert(nos_notify_wait_obj(&notif, &bits, NOS_NO_WAIT) == -EAGAIN);
    assert(bits == 0ULL);

    /* 2. 累加多个不同事件位 */
    assert(nos_notify_signal_obj(&notif, 1ULL << 0) == 0);
    assert(nos_notify_signal_obj(&notif, 1ULL << 3) == 0);
    assert(nos_notify_signal_obj(&notif, 1ULL << 63) == 0);

    /* 3. 同位高频触发合并去抖 (重复触发 bit 3 与 bit 0) */
    assert(nos_notify_signal_obj(&notif, 1ULL << 3) == 0);
    assert(nos_notify_signal_obj(&notif, 1ULL << 0) == 0);
    assert(notif.pending_bits == ((1ULL << 63) | (1ULL << 3) | (1ULL << 0)));

    /* 4. 非阻塞原子读清：提取累积事件并清零内部位图 */
    assert(nos_notify_wait_obj(&notif, &bits, NOS_NO_WAIT) == 0);
    assert(bits == ((1ULL << 63) | (1ULL << 3) | (1ULL << 0)));
    assert(notif.pending_bits == 0ULL);

    /* 5. 再次读清应立即返回 -EAGAIN，证明已完全消费 */
    bits = 0ULL;
    assert(nos_notify_wait_obj(&notif, &bits, NOS_NO_WAIT) == -EAGAIN);
    assert(bits == 0ULL);

    puts("[PASS] test_notify_coalescing_and_read_clear");
}

/**
 * 测试 3: 单任务阻塞休眠与直接交接唤醒
 */
static void test_notify_blocking_wait_single_waiter(void)
{
    struct nos_notification notif;
    struct mock_task_ctx waiter;
    pthread_t th;
    const uint64_t test_bits = 0x8000000000000001ULL;

    assert(nos_notification_init(&notif) == 0);

    init_mock_ctx(&waiter, "single_waiter", 15);
    waiter.target_notif = &notif;
    waiter.timeout = NOS_WAIT_FOREVER;

    assert(pthread_create(&th, NULL, task_wait_worker, &waiter) == 0);
    wait_for_task_in_wait(&waiter);

    assert(nos_wait_queue_count(&notif.wait_queue) == 1U);
    assert(notif.pending_bits == 0ULL);

    /* 触发事件，验证直接交接给等待任务并清空内部 pending */
    assert(nos_notify_signal_obj(&notif, test_bits) == 0);

    pthread_join(th, NULL);

    assert(waiter.wait_result == 0);
    assert(waiter.out_bits == test_bits);
    assert(notif.pending_bits == 0ULL);
    assert(nos_wait_queue_empty(&notif.wait_queue) == true);

    destroy_mock_ctx(&waiter);
    puts("[PASS] test_notify_blocking_wait_single_waiter");
}

/**
 * 测试 4: 多任务按实时优先级排队唤醒与事件分发
 */
static void test_notify_multi_waiter_prio_scheduling(void)
{
    struct nos_notification notif;
    struct mock_task_ctx t_low;
    struct mock_task_ctx t_high;
    pthread_t th_low, th_high;

    assert(nos_notification_init(&notif) == 0);

    init_mock_ctx(&t_low, "low_prio", 25);     /* 优先级 25 (低优先级) */
    t_low.target_notif = &notif;
    t_low.timeout = NOS_WAIT_FOREVER;

    init_mock_ctx(&t_high, "high_prio", 10);   /* 优先级 10 (高优先级) */
    t_high.target_notif = &notif;
    t_high.timeout = NOS_WAIT_FOREVER;

    /* 先让低优先级任务排队 */
    assert(pthread_create(&th_low, NULL, task_wait_worker, &t_low) == 0);
    wait_for_task_in_wait(&t_low);

    /* 再让高优先级任务排队 */
    assert(pthread_create(&th_high, NULL, task_wait_worker, &t_high) == 0);
    wait_for_task_in_wait(&t_high);

    assert(nos_wait_queue_count(&notif.wait_queue) == 2U);

    /* 第一次 signal：应优先唤醒高优先级的 t_high */
    assert(nos_notify_signal_obj(&notif, 0xAAAA0000ULL) == 0);
    pthread_join(th_high, NULL);

    assert(t_high.wait_result == 0);
    assert(t_high.out_bits == 0xAAAA0000ULL);

    /* t_low 仍处于阻塞排队中 */
    assert(nos_wait_queue_count(&notif.wait_queue) == 1U);

    /* 第二次 signal：唤醒低优先级的 t_low */
    assert(nos_notify_signal_obj(&notif, 0x5555ULL) == 0);
    pthread_join(th_low, NULL);

    assert(t_low.wait_result == 0);
    assert(t_low.out_bits == 0x5555ULL);
    assert(nos_wait_queue_empty(&notif.wait_queue) == true);

    destroy_mock_ctx(&t_low);
    destroy_mock_ctx(&t_high);
    puts("[PASS] test_notify_multi_waiter_prio_scheduling");
}

/**
 * 测试 5: 超时唤醒与取消 (返回 -ETIMEDOUT)
 */
static void test_notify_timeout_behavior(void)
{
    struct nos_notification notif;
    struct mock_task_ctx t_timeout;
    pthread_t th;
    uint64_t bits = 0ULL;

    assert(nos_notification_init(&notif) == 0);

    init_mock_ctx(&t_timeout, "timeout_task", 12);
    t_timeout.target_notif = &notif;
    t_timeout.timeout = 50U;

    assert(pthread_create(&th, NULL, task_wait_worker, &t_timeout) == 0);
    wait_for_task_in_wait(&t_timeout);

    assert(nos_wait_queue_count(&notif.wait_queue) == 1U);

    /* 模拟定时器到期触发唤醒 */
    pthread_mutex_lock(&t_timeout.mtx);
    t_timeout.woken = true;
    pthread_cond_broadcast(&t_timeout.cv);
    pthread_mutex_unlock(&t_timeout.mtx);

    pthread_join(th, NULL);

    assert(t_timeout.wait_result == -ETIMEDOUT);
    assert(t_timeout.out_bits == 0ULL);
    assert(nos_wait_queue_empty(&notif.wait_queue) == true);

    /* 超时后通知对象功能完好 */
    assert(nos_notify_signal_obj(&notif, 0x1234ULL) == 0);
    assert(nos_notify_wait_obj(&notif, &bits, NOS_NO_WAIT) == 0);
    assert(bits == 0x1234ULL);

    destroy_mock_ctx(&t_timeout);
    puts("[PASS] test_notify_timeout_behavior");
}

/**
 * 测试 6: 关闭通知对象排空所有等待者 (返回 -EPIPE)
 */
static void test_notify_close_drains_waiters(void)
{
    struct nos_notification notif;
    struct mock_task_ctx t1, t2;
    pthread_t th1, th2;
    uint64_t bits = 0ULL;

    assert(nos_notification_init(&notif) == 0);

    init_mock_ctx(&t1, "waiter_1", 10);
    t1.target_notif = &notif;
    t1.timeout = NOS_WAIT_FOREVER;

    init_mock_ctx(&t2, "waiter_2", 20);
    t2.target_notif = &notif;
    t2.timeout = NOS_WAIT_FOREVER;

    assert(pthread_create(&th1, NULL, task_wait_worker, &t1) == 0);
    wait_for_task_in_wait(&t1);

    assert(pthread_create(&th2, NULL, task_wait_worker, &t2) == 0);
    wait_for_task_in_wait(&t2);

    assert(nos_wait_queue_count(&notif.wait_queue) == 2U);

    /* 关闭通知对象 */
    assert(nos_notification_close(&notif) == 0);

    pthread_join(th1, NULL);
    pthread_join(th2, NULL);

    assert(t1.wait_result == -EPIPE);
    assert(t2.wait_result == -EPIPE);
    assert(nos_wait_queue_empty(&notif.wait_queue) == true);

    /* 幂等性测试：再次关闭返回 0 */
    assert(nos_notification_close(&notif) == 0);

    /* 关闭后后续操作均拒绝并返回 -EPIPE */
    assert(nos_notify_wait_obj(&notif, &bits, NOS_WAIT_FOREVER) == -EPIPE);
    assert(nos_notify_wait_obj(&notif, &bits, NOS_NO_WAIT) == -EPIPE);
    assert(nos_notify_signal_obj(&notif, 0x1ULL) == -EPIPE);

    destroy_mock_ctx(&t1);
    destroy_mock_ctx(&t2);
    puts("[PASS] test_notify_close_drains_waiters");
}

/**
 * 测试 7: 无任务上下文 (current == NULL) 下的非阻塞行为
 */
static void test_notify_no_task_context(void)
{
    struct nos_notification notif;
    uint64_t bits = 0ULL;

    assert(nos_notification_init(&notif) == 0);

    s_current_task = NULL;

    /* 空位图非阻塞等待返回 -EAGAIN */
    assert(nos_notify_wait_obj(&notif, &bits, NOS_NO_WAIT) == -EAGAIN);

    /* 触发位图 */
    assert(nos_notify_signal_obj(&notif, 0xF00DULL) == 0);

    /* 读取位图 */
    assert(nos_notify_wait_obj(&notif, &bits, NOS_NO_WAIT) == 0);
    assert(bits == 0xF00DULL);
    assert(notif.pending_bits == 0ULL);

    /* 关闭测试 */
    assert(nos_notification_close(&notif) == 0);
    assert(nos_notify_wait_obj(&notif, &bits, NOS_NO_WAIT) == -EPIPE);

    puts("[PASS] test_notify_no_task_context");
}

int main(void)
{
    puts("=== Running IPC Bitmask Notification Tests ===");

    test_notify_init_and_param_boundary();
    test_notify_coalescing_and_read_clear();
    test_notify_blocking_wait_single_waiter();
    test_notify_multi_waiter_prio_scheduling();
    test_notify_timeout_behavior();
    test_notify_close_drains_waiters();
    test_notify_no_task_context();

    puts("All IPC Notification tests passed successfully!");
    return 0;
}
