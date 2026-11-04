/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * 宿主端测试：32 位计数信号量 (nos_sem) 与通用等待底座集成
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
#include <nos/sem.h>

/* 模拟任务运行上下文 */
struct mock_task_ctx {
    struct task_struct task;
    pthread_mutex_t    mtx;
    pthread_cond_t     cv;
    bool               woken;
    bool               in_wait;
    int                take_result;
    struct nos_sem    *target_sem;
    nos_timeout_t      timeout;
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
    ctx->take_result = 0;
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

static void *task_take_worker(void *arg)
{
    struct mock_task_ctx *ctx = (struct mock_task_ctx *)arg;
    s_current_task = &ctx->task;
    ctx->take_result = nos_sem_take(ctx->target_sem, ctx->timeout);
    return NULL;
}

/**
 * 测试 1: initial 与 limit 边界校验及非法参数防护
 */
static void test_sem_init_and_param_boundary(void)
{
    struct nos_sem sem;

    /* 空指针参数校验 */
    assert(nos_sem_init(NULL, 0, 1) == -EINVAL);
    assert(nos_sem_take(NULL, 0) == -EINVAL);
    assert(nos_sem_trytake(NULL) == -EINVAL);
    assert(nos_sem_give(NULL) == -EINVAL);
    assert(nos_sem_close(NULL) == -EINVAL);

    /* limit 为 0 校验 */
    assert(nos_sem_init(&sem, 0, 0) == -EINVAL);

    /* initial 超过 limit 校验 */
    assert(nos_sem_init(&sem, 5, 2) == -EINVAL);

    /* 合法初始化：initial == 0, limit == 10 */
    assert(nos_sem_init(&sem, 0, 10) == 0);
    assert(sem.count == 0U);
    assert(sem.limit == 10U);
    assert(nos_wait_queue_empty(&sem.wait_queue) == true);
    assert(nos_object_is_closed(&sem.object) == false);

    /* 合法初始化：initial == limit */
    assert(nos_sem_init(&sem, 5, 5) == 0);
    assert(sem.count == 5U);
    assert(sem.limit == 5U);

    puts("[PASS] test_sem_init_and_param_boundary");
}

/**
 * 测试 2: 正常 give/take 令牌守恒与增减正确性
 */
static void test_sem_give_take_conservation(void)
{
    struct nos_sem sem;

    assert(nos_sem_init(&sem, 0, 3) == 0);

    /* 初始令牌为 0，非阻塞获取失败 */
    assert(nos_sem_trytake(&sem) == -EAGAIN);
    assert(sem.count == 0U);

    /* 释放 1 个令牌并获取 */
    assert(nos_sem_give(&sem) == 0);
    assert(sem.count == 1U);
    assert(nos_sem_trytake(&sem) == 0);
    assert(sem.count == 0U);
    assert(nos_sem_trytake(&sem) == -EAGAIN);

    /* 释放 3 个令牌到达上限 */
    assert(nos_sem_give(&sem) == 0);
    assert(nos_sem_give(&sem) == 0);
    assert(nos_sem_give(&sem) == 0);
    assert(sem.count == 3U);

    /* 依次消耗 3 个令牌 */
    assert(nos_sem_trytake(&sem) == 0);
    assert(sem.count == 2U);
    assert(nos_sem_take(&sem, NOS_NO_WAIT) == 0);
    assert(sem.count == 1U);
    assert(nos_sem_trytake(&sem) == 0);
    assert(sem.count == 0U);
    assert(nos_sem_trytake(&sem) == -EAGAIN);

    puts("[PASS] test_sem_give_take_conservation");
}

/**
 * 测试 3: 超过 limit 时 give 报错 -EOVERFLOW (拒绝静默饱和溢出)
 */
static void test_sem_overflow_rejection(void)
{
    struct nos_sem sem;

    assert(nos_sem_init(&sem, 2, 2) == 0);
    assert(sem.count == 2U);

    /* 已达 limit 上限，继续释放必须报错 -EOVERFLOW，且令牌数不得改变 */
    assert(nos_sem_give(&sem) == -EOVERFLOW);
    assert(sem.count == 2U);

    /* 获取一个令牌后恢复可释放状态 */
    assert(nos_sem_trytake(&sem) == 0);
    assert(sem.count == 1U);

    assert(nos_sem_give(&sem) == 0);
    assert(sem.count == 2U);

    /* 再次溢出尝试 */
    assert(nos_sem_give(&sem) == -EOVERFLOW);
    assert(sem.count == 2U);

    puts("[PASS] test_sem_overflow_rejection");
}

/**
 * 测试 4: trytake 在无令牌时返回 -EAGAIN
 */
static void test_sem_trytake_no_wait(void)
{
    struct nos_sem sem;

    assert(nos_sem_init(&sem, 0, 1) == 0);

    /* 无可用令牌且非阻塞 */
    assert(nos_sem_trytake(&sem) == -EAGAIN);
    assert(nos_sem_take(&sem, NOS_NO_WAIT) == -EAGAIN);

    /* 产生令牌后成功获取 */
    assert(nos_sem_give(&sem) == 0);
    assert(nos_sem_trytake(&sem) == 0);

    /* 再次获取恢复 -EAGAIN */
    assert(nos_sem_trytake(&sem) == -EAGAIN);

    puts("[PASS] test_sem_trytake_no_wait");
}

/**
 * 测试 5: 多任务优先级排队与直接令牌交接唤醒 (严格保持 count 不变)
 */
static void test_sem_priority_queue_and_direct_handoff(void)
{
    struct nos_sem sem;
    struct mock_task_ctx t_low, t_mid, t_high;
    pthread_t th_low, th_mid, th_high;

    assert(nos_sem_init(&sem, 0, 10) == 0);

    init_mock_ctx(&t_low, "low", 20);     /* 优先级 20 (低优先级) */
    init_mock_ctx(&t_mid, "mid", 10);     /* 优先级 10 (中优先级) */
    init_mock_ctx(&t_high, "high", 5);    /* 优先级 5 (高优先级) */

    t_low.target_sem = &sem;
    t_low.timeout = NOS_WAIT_FOREVER;
    t_mid.target_sem = &sem;
    t_mid.timeout = NOS_WAIT_FOREVER;
    t_high.target_sem = &sem;
    t_high.timeout = NOS_WAIT_FOREVER;

    /* 先启动低优先级任务并等待其挂入等待队列 */
    assert(pthread_create(&th_low, NULL, task_take_worker, &t_low) == 0);
    wait_for_task_in_wait(&t_low);

    /* 启动中优先级任务并等待其挂入等待队列 */
    assert(pthread_create(&th_mid, NULL, task_take_worker, &t_mid) == 0);
    wait_for_task_in_wait(&t_mid);

    /* 启动高优先级任务并等待其挂入等待队列 */
    assert(pthread_create(&th_high, NULL, task_take_worker, &t_high) == 0);
    wait_for_task_in_wait(&t_high);

    /* 校验此时等待队列包含 3 个任务 */
    assert(nos_wait_queue_count(&sem.wait_queue) == 3U);

    /* 校验队首必须是最高优先级的 t_high (优先级 5) */
    assert(nos_wait_queue_first(&sem.wait_queue) == &t_high.task.wait_entry);

    /*
     * 第一次 give:
     * 必须直接交接给最高优先级的 t_high，sem.count 保持为 0，t_high 成功返回 0
     */
    assert(nos_sem_give(&sem) == 0);
    assert(sem.count == 0U);

    pthread_join(th_high, NULL);
    assert(t_high.take_result == 0);
    assert(nos_wait_queue_count(&sem.wait_queue) == 2U);

    /* 校验当前队首跃迁为中优先级的 t_mid (优先级 10) */
    assert(nos_wait_queue_first(&sem.wait_queue) == &t_mid.task.wait_entry);

    /*
     * 第二次 give:
     * 交接给 t_mid，sem.count 仍保持 0
     */
    assert(nos_sem_give(&sem) == 0);
    assert(sem.count == 0U);

    pthread_join(th_mid, NULL);
    assert(t_mid.take_result == 0);
    assert(nos_wait_queue_count(&sem.wait_queue) == 1U);

    /* 校验当前队首为最后的 t_low (优先级 20) */
    assert(nos_wait_queue_first(&sem.wait_queue) == &t_low.task.wait_entry);

    /*
     * 第三次 give:
     * 交接给 t_low，sem.count 仍保持 0
     */
    assert(nos_sem_give(&sem) == 0);
    assert(sem.count == 0U);

    pthread_join(th_low, NULL);
    assert(t_low.take_result == 0);
    assert(nos_wait_queue_empty(&sem.wait_queue) == true);

    /*
     * 第四次 give:
     * 此时无任何等待者，sem.count 累加至 1
     */
    assert(nos_sem_give(&sem) == 0);
    assert(sem.count == 1U);

    destroy_mock_ctx(&t_low);
    destroy_mock_ctx(&t_mid);
    destroy_mock_ctx(&t_high);

    puts("[PASS] test_sem_priority_queue_and_direct_handoff");
}

/**
 * 测试 6: close 时所有等待者被唤醒且返回 -EPIPE
 */
static void test_sem_close_drains_waiters(void)
{
    struct nos_sem sem;
    struct mock_task_ctx t1, t2, t3;
    pthread_t th1, th2, th3;

    assert(nos_sem_init(&sem, 0, 5) == 0);

    init_mock_ctx(&t1, "t1", 10);
    init_mock_ctx(&t2, "t2", 15);
    init_mock_ctx(&t3, "t3", 20);

    t1.target_sem = &sem;
    t1.timeout = NOS_WAIT_FOREVER;
    t2.target_sem = &sem;
    t2.timeout = NOS_WAIT_FOREVER;
    t3.target_sem = &sem;
    t3.timeout = NOS_WAIT_FOREVER;

    assert(pthread_create(&th1, NULL, task_take_worker, &t1) == 0);
    assert(pthread_create(&th2, NULL, task_take_worker, &t2) == 0);
    assert(pthread_create(&th3, NULL, task_take_worker, &t3) == 0);

    wait_for_task_in_wait(&t1);
    wait_for_task_in_wait(&t2);
    wait_for_task_in_wait(&t3);

    assert(nos_wait_queue_count(&sem.wait_queue) == 3U);

    /* 关闭信号量 */
    assert(nos_sem_close(&sem) == 0);
    assert(nos_object_is_closed(&sem.object) == true);

    /* 所有等待线程均被唤醒，且返回 -EPIPE */
    pthread_join(th1, NULL);
    pthread_join(th2, NULL);
    pthread_join(th3, NULL);

    assert(t1.take_result == -EPIPE);
    assert(t2.take_result == -EPIPE);
    assert(t3.take_result == -EPIPE);

    assert(nos_wait_queue_empty(&sem.wait_queue) == true);

    /* 重复关闭保持幂等性返回 0 */
    assert(nos_sem_close(&sem) == 0);

    /* 关闭后禁止接纳新操作 */
    assert(nos_sem_take(&sem, NOS_WAIT_FOREVER) == -EPIPE);
    assert(nos_sem_trytake(&sem) == -EPIPE);
    assert(nos_sem_give(&sem) == -EPIPE);

    destroy_mock_ctx(&t1);
    destroy_mock_ctx(&t2);
    destroy_mock_ctx(&t3);

    puts("[PASS] test_sem_close_drains_waiters");
}

/**
 * 测试 7: 超时唤醒与取消 (返回 -ETIMEDOUT 且令牌不丢失)
 */
static void test_sem_timeout_behavior(void)
{
    struct nos_sem sem;
    struct mock_task_ctx t_timeout;
    pthread_t th;

    assert(nos_sem_init(&sem, 0, 2) == 0);

    init_mock_ctx(&t_timeout, "timeout_task", 10);
    t_timeout.target_sem = &sem;
    t_timeout.timeout = 50ULL;

    assert(pthread_create(&th, NULL, task_take_worker, &t_timeout) == 0);
    wait_for_task_in_wait(&t_timeout);

    assert(nos_wait_queue_count(&sem.wait_queue) == 1U);

    /* 模拟定时器触发唤醒（未持有令牌完成，仅唤醒以让任务进行单点超时判定） */
    pthread_mutex_lock(&t_timeout.mtx);
    t_timeout.woken = true;
    pthread_cond_broadcast(&t_timeout.cv);
    pthread_mutex_unlock(&t_timeout.mtx);

    pthread_join(th, NULL);
    assert(t_timeout.take_result == -ETIMEDOUT);
    assert(nos_wait_queue_empty(&sem.wait_queue) == true);

    /* 之后正常释放令牌不受影响 */
    assert(nos_sem_give(&sem) == 0);
    assert(sem.count == 1U);
    assert(nos_sem_trytake(&sem) == 0);
    assert(sem.count == 0U);

    destroy_mock_ctx(&t_timeout);

    puts("[PASS] test_sem_timeout_behavior");
}

int main(void)
{
    puts("=== Running IPC Semaphore Foundation Tests ===");

    test_sem_init_and_param_boundary();
    test_sem_give_take_conservation();
    test_sem_overflow_rejection();
    test_sem_trytake_no_wait();
    test_sem_priority_queue_and_direct_handoff();
    test_sem_close_drains_waiters();
    test_sem_timeout_behavior();

    puts("All IPC semaphore tests PASS");
    return 0;
}
