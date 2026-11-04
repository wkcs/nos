/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * 宿主端测试：完成量 (nos_completion) 核心行为验证
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
#include <nos/completion.h>

/* 模拟任务运行上下文 */
struct mock_task_ctx {
    struct task_struct      task;
    pthread_mutex_t         mtx;
    pthread_cond_t          cv;
    bool                    woken;
    bool                    in_wait;
    int                     wait_result;
    struct nos_completion  *target_comp;
    nos_timeout_t           timeout;
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
    ctx->wait_result = 0;
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
    ctx->wait_result = nos_completion_wait(ctx->target_comp, ctx->timeout);
    return NULL;
}

/**
 * 测试 1: 参数边界校验与合法初始化状态验证
 */
static void test_completion_init_and_param_boundary(void)
{
    struct nos_completion comp;

    /* 空指针参数校验 */
    assert(nos_completion_init(NULL) == -EINVAL);
    assert(nos_completion_wait(NULL, 0) == -EINVAL);
    assert(nos_complete(NULL) == -EINVAL);
    assert(nos_complete_all(NULL) == -EINVAL);
    assert(nos_completion_reset(NULL) == -EINVAL);
    assert(nos_completion_close(NULL) == -EINVAL);

    /* 正常初始化 */
    assert(nos_completion_init(&comp) == 0);
    assert(comp.done == 0U);
    assert(nos_wait_queue_empty(&comp.wait_queue) == true);
    assert(nos_object_is_closed(&comp.object) == false);

    puts("[PASS] test_completion_init_and_param_boundary");
}

/**
 * 测试 2: 非阻塞模式与预先 complete 计数累加与消耗
 */
static void test_completion_nonblocking_and_precomplete(void)
{
    struct nos_completion comp;

    assert(nos_completion_init(&comp) == 0);

    /* 初始未完成时，非阻塞 wait 立即返回 -EAGAIN */
    assert(nos_completion_wait(&comp, NOS_NO_WAIT) == -EAGAIN);
    assert(comp.done == 0U);

    /* 预先执行两次 complete */
    assert(nos_complete(&comp) == 0);
    assert(comp.done == 1U);
    assert(nos_complete(&comp) == 0);
    assert(comp.done == 2U);

    /* 连续两次非阻塞 wait 消耗两次完成计数并返回 0 */
    assert(nos_completion_wait(&comp, NOS_NO_WAIT) == 0);
    assert(comp.done == 1U);
    assert(nos_completion_wait(&comp, NOS_NO_WAIT) == 0);
    assert(comp.done == 0U);

    /* 计数归零后再次非阻塞 wait 返回 -EAGAIN */
    assert(nos_completion_wait(&comp, NOS_NO_WAIT) == -EAGAIN);

    puts("[PASS] test_completion_nonblocking_and_precomplete");
}

/**
 * 测试 3: 单次 complete 唤醒单个等待者 (严格遵循 FIFO 次序)
 */
static void test_completion_single_complete_fifo_wakeup(void)
{
    struct nos_completion comp;
    struct mock_task_ctx t1, t2, t3;
    pthread_t th1, th2, th3;

    assert(nos_completion_init(&comp) == 0);

    /* 创建 3 个不同优先级的等待任务，但按 t1 -> t2 -> t3 次序入队 */
    init_mock_ctx(&t1, "t1", 20); /* 优先级 20 */
    init_mock_ctx(&t2, "t2", 10); /* 优先级 10 */
    init_mock_ctx(&t3, "t3", 5);  /* 优先级 5 */

    t1.target_comp = &comp;
    t1.timeout = NOS_WAIT_FOREVER;
    t2.target_comp = &comp;
    t2.timeout = NOS_WAIT_FOREVER;
    t3.target_comp = &comp;
    t3.timeout = NOS_WAIT_FOREVER;

    /* t1 先入队 */
    assert(pthread_create(&th1, NULL, task_wait_worker, &t1) == 0);
    wait_for_task_in_wait(&t1);

    /* t2 入队 */
    assert(pthread_create(&th2, NULL, task_wait_worker, &t2) == 0);
    wait_for_task_in_wait(&t2);

    /* t3 入队 */
    assert(pthread_create(&th3, NULL, task_wait_worker, &t3) == 0);
    wait_for_task_in_wait(&t3);

    assert(nos_wait_queue_count(&comp.wait_queue) == 3U);

    /*
     * 校验 FIFO 次序：即使 t3 优先级最高，队列首节点依然必须是先到的 t1
     */
    assert(nos_wait_queue_first(&comp.wait_queue) == &t1.task.wait_entry);

    /* 第一次 complete 唤醒 t1 */
    assert(nos_complete(&comp) == 0);
    pthread_join(th1, NULL);
    assert(t1.wait_result == 0);
    assert(comp.done == 0U);
    assert(nos_wait_queue_count(&comp.wait_queue) == 2U);
    assert(nos_wait_queue_first(&comp.wait_queue) == &t2.task.wait_entry);

    /* 第二次 complete 唤醒 t2 */
    assert(nos_complete(&comp) == 0);
    pthread_join(th2, NULL);
    assert(t2.wait_result == 0);
    assert(comp.done == 0U);
    assert(nos_wait_queue_count(&comp.wait_queue) == 1U);
    assert(nos_wait_queue_first(&comp.wait_queue) == &t3.task.wait_entry);

    /* 第三次 complete 唤醒 t3 */
    assert(nos_complete(&comp) == 0);
    pthread_join(th3, NULL);
    assert(t3.wait_result == 0);
    assert(comp.done == 0U);
    assert(nos_wait_queue_empty(&comp.wait_queue) == true);

    destroy_mock_ctx(&t1);
    destroy_mock_ctx(&t2);
    destroy_mock_ctx(&t3);

    puts("[PASS] test_completion_single_complete_fifo_wakeup");
}

/**
 * 测试 4: 广播 complete_all 唤醒所有活动等待者，且后续 wait 立即无阻塞返回 0
 */
static void test_completion_broadcast_complete_all(void)
{
    struct nos_completion comp;
    struct mock_task_ctx t1, t2, t3;
    pthread_t th1, th2, th3;

    assert(nos_completion_init(&comp) == 0);

    init_mock_ctx(&t1, "t1", 10);
    init_mock_ctx(&t2, "t2", 15);
    init_mock_ctx(&t3, "t3", 20);

    t1.target_comp = &comp;
    t1.timeout = NOS_WAIT_FOREVER;
    t2.target_comp = &comp;
    t2.timeout = NOS_WAIT_FOREVER;
    t3.target_comp = &comp;
    t3.timeout = NOS_WAIT_FOREVER;

    assert(pthread_create(&th1, NULL, task_wait_worker, &t1) == 0);
    assert(pthread_create(&th2, NULL, task_wait_worker, &t2) == 0);
    assert(pthread_create(&th3, NULL, task_wait_worker, &t3) == 0);

    wait_for_task_in_wait(&t1);
    wait_for_task_in_wait(&t2);
    wait_for_task_in_wait(&t3);

    assert(nos_wait_queue_count(&comp.wait_queue) == 3U);

    /* 执行 complete_all 广播唤醒 */
    assert(nos_complete_all(&comp) == 0);
    assert(comp.done == UINT32_MAX);

    pthread_join(th1, NULL);
    pthread_join(th2, NULL);
    pthread_join(th3, NULL);

    assert(t1.wait_result == 0);
    assert(t2.wait_result == 0);
    assert(t3.wait_result == 0);
    assert(nos_wait_queue_empty(&comp.wait_queue) == true);

    /* 广播完成状态下，后续任意 wait 调用立即无阻塞返回 0 */
    assert(nos_completion_wait(&comp, NOS_NO_WAIT) == 0);
    assert(nos_completion_wait(&comp, NOS_WAIT_FOREVER) == 0);
    assert(nos_completion_wait(&comp, 100ULL) == 0);
    assert(comp.done == UINT32_MAX);

    /* 在广播状态下调用 nos_complete 保持 UINT32_MAX 且返回 0 */
    assert(nos_complete(&comp) == 0);
    assert(comp.done == UINT32_MAX);

    destroy_mock_ctx(&t1);
    destroy_mock_ctx(&t2);
    destroy_mock_ctx(&t3);

    puts("[PASS] test_completion_broadcast_complete_all");
}

/**
 * 测试 5: reset 在无等待者时成功重置，在有等待者时返回 -EBUSY
 */
static void test_completion_reset_behavior(void)
{
    struct nos_completion comp;
    struct mock_task_ctx t_waiter;
    pthread_t th;

    assert(nos_completion_init(&comp) == 0);

    /* 广播完成后重置验证 */
    assert(nos_complete_all(&comp) == 0);
    assert(comp.done == UINT32_MAX);

    /* 无等待者时，reset 成功清零 */
    assert(nos_completion_reset(&comp) == 0);
    assert(comp.done == 0U);

    /* 重置后恢复未完成阻塞/EAGAIN 状态 */
    assert(nos_completion_wait(&comp, NOS_NO_WAIT) == -EAGAIN);

    /* 启动等待线程进入排队 */
    init_mock_ctx(&t_waiter, "waiter", 10);
    t_waiter.target_comp = &comp;
    t_waiter.timeout = NOS_WAIT_FOREVER;

    assert(pthread_create(&th, NULL, task_wait_worker, &t_waiter) == 0);
    wait_for_task_in_wait(&t_waiter);

    assert(nos_wait_queue_count(&comp.wait_queue) == 1U);

    /* 有等待者排队时，reset 必须拒绝并返回 -EBUSY */
    assert(nos_completion_reset(&comp) == -EBUSY);

    /* 唤醒并回收等待线程 */
    assert(nos_complete(&comp) == 0);
    pthread_join(th, NULL);
    assert(t_waiter.wait_result == 0);

    /* 此时队列清空，reset 再次成功 */
    assert(nos_completion_reset(&comp) == 0);
    assert(comp.done == 0U);

    destroy_mock_ctx(&t_waiter);

    puts("[PASS] test_completion_reset_behavior");
}

/**
 * 测试 6: close 唤醒所有活动等待者返回 -EPIPE 且保持幂等性
 */
static void test_completion_close_drains_waiters(void)
{
    struct nos_completion comp;
    struct mock_task_ctx t1, t2;
    pthread_t th1, th2;

    assert(nos_completion_init(&comp) == 0);

    init_mock_ctx(&t1, "t1", 10);
    init_mock_ctx(&t2, "t2", 15);

    t1.target_comp = &comp;
    t1.timeout = NOS_WAIT_FOREVER;
    t2.target_comp = &comp;
    t2.timeout = NOS_WAIT_FOREVER;

    assert(pthread_create(&th1, NULL, task_wait_worker, &t1) == 0);
    assert(pthread_create(&th2, NULL, task_wait_worker, &t2) == 0);

    wait_for_task_in_wait(&t1);
    wait_for_task_in_wait(&t2);

    assert(nos_wait_queue_count(&comp.wait_queue) == 2U);

    /* 关闭完成量 */
    assert(nos_completion_close(&comp) == 0);
    assert(nos_object_is_closed(&comp.object) == true);

    pthread_join(th1, NULL);
    pthread_join(th2, NULL);

    /* 所有等待者均获得 -EPIPE 返回值 */
    assert(t1.wait_result == -EPIPE);
    assert(t2.wait_result == -EPIPE);
    assert(nos_wait_queue_empty(&comp.wait_queue) == true);

    /* 重复关闭保持幂等性返回 0 */
    assert(nos_completion_close(&comp) == 0);

    /* 关闭后所有操作均返回 -EPIPE */
    assert(nos_completion_wait(&comp, NOS_WAIT_FOREVER) == -EPIPE);
    assert(nos_completion_wait(&comp, NOS_NO_WAIT) == -EPIPE);
    assert(nos_complete(&comp) == -EPIPE);
    assert(nos_complete_all(&comp) == -EPIPE);
    assert(nos_completion_reset(&comp) == -EPIPE);

    destroy_mock_ctx(&t1);
    destroy_mock_ctx(&t2);

    puts("[PASS] test_completion_close_drains_waiters");
}

/**
 * 测试 7: 超时唤醒与取消 (返回 -ETIMEDOUT)
 */
static void test_completion_timeout_behavior(void)
{
    struct nos_completion comp;
    struct mock_task_ctx t_timeout;
    pthread_t th;

    assert(nos_completion_init(&comp) == 0);

    init_mock_ctx(&t_timeout, "timeout_task", 10);
    t_timeout.target_comp = &comp;
    t_timeout.timeout = 50ULL;

    assert(pthread_create(&th, NULL, task_wait_worker, &t_timeout) == 0);
    wait_for_task_in_wait(&t_timeout);

    assert(nos_wait_queue_count(&comp.wait_queue) == 1U);

    /* 模拟定时器超时触发唤醒 */
    pthread_mutex_lock(&t_timeout.mtx);
    t_timeout.woken = true;
    pthread_cond_broadcast(&t_timeout.cv);
    pthread_mutex_unlock(&t_timeout.mtx);

    pthread_join(th, NULL);
    assert(t_timeout.wait_result == -ETIMEDOUT);
    assert(nos_wait_queue_empty(&comp.wait_queue) == true);

    /* 超时退出后，完成量仍可正常工作 */
    assert(nos_complete(&comp) == 0);
    assert(comp.done == 1U);
    assert(nos_completion_wait(&comp, NOS_NO_WAIT) == 0);
    assert(comp.done == 0U);

    destroy_mock_ctx(&t_timeout);

    puts("[PASS] test_completion_timeout_behavior");
}

int main(void)
{
    puts("=== Running IPC Completion Primitive Tests ===");

    test_completion_init_and_param_boundary();
    test_completion_nonblocking_and_precomplete();
    test_completion_single_complete_fifo_wakeup();
    test_completion_broadcast_complete_all();
    test_completion_reset_behavior();
    test_completion_close_drains_waiters();
    test_completion_timeout_behavior();

    puts("All IPC completion tests PASS");
    return 0;
}
