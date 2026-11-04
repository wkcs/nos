/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * 宿主端测试：通用等待底座 (nos_wait_entry / nos_wait_queue)
 * 与单点仲裁状态机、延后唤醒批次 (nos_wake_batch)。
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <kernel/spinlock.h>
#include <kernel/task.h>
#include <nos/refcount.h>
#include <nos/object.h>
#include <nos/timeout.h>
#include <nos/wait.h>

/* 模拟单调时钟 */
static uint64_t s_mock_ticks = 1000ULL;

uint64_t cpu_run_ticks(void)
{
    return s_mock_ticks;
}

/* 跟踪唤醒调用的状态 */
static int s_wakeup_count = 0;
static struct task_struct *s_last_woken_task = NULL;
static bool s_object_lock_held_during_dispatch = false;
static spinlock_t *s_monitored_obj_lock = NULL;

/* 覆盖/实现任务唤醒钩子 */
void nos_task_wake_up_locked(struct task_struct *task)
{
    s_wakeup_count++;
    s_last_woken_task = task;
    if (task != NULL) {
        task->status = 0x02; /* TASK_READY */
    }

    if (s_monitored_obj_lock != NULL && *s_monitored_obj_lock != 0) {
        s_object_lock_held_during_dispatch = true;
    }
}

static void init_mock_task(struct task_struct *task, const char *name, uint8_t prio)
{
    memset(task, 0, sizeof(*task));
    task->name = name;
    task->current_priority = prio;
    task->init_priority = prio;
    task->status = 0x20; /* TASK_BLOCKED */
    spin_lock_init(&task->lock);
    nos_refcount_init(&task->refcount, 1U);
    nos_wait_entry_init(&task->wait_entry, task);
}

/**
 * 测试 1: wait_entry 初始化与参数边界
 */
static void test_wait_entry_init_and_params(void)
{
    struct task_struct task;
    struct nos_wait_entry entry;
    struct nos_object obj;
    int rc;

    init_mock_task(&task, "task_init", 10);
    nos_object_init(&obj, true);

    /* 校验初始化状态 */
    nos_wait_entry_init(&entry, &task);
    assert(entry.task == &task);
    assert(entry.state == NOS_WAIT_STATE_IDLE);
    assert(entry.epoch == 0ULL);
    assert(entry.result == 0);
    assert(entry.deadline == 0ULL);
    assert(entry.waiting_obj == NULL);
    assert(list_empty(&entry.wait_node));
    assert(list_empty(&entry.wake_node));

    /* 空指针参数校验 */
    assert(nos_wait_prepare(NULL, &obj, 2000ULL) == -EINVAL);
    assert(nos_wait_prepare(&entry, NULL, 2000ULL) == -EINVAL);
    assert(nos_wait_complete_locked(NULL, 0, NULL) == -EINVAL);

    /* IDLE 状态下直接完成必须被拒绝 (非 WAITING) */
    assert(nos_wait_complete_locked(&entry, 0, NULL) == -EALREADY);

    puts("[PASS] test_wait_entry_init_and_params");
}

/**
 * 测试 2: 状态机流转 (IDLE -> WAITING -> SUCCEEDED / TIMEDOUT / CLOSED / CANCELLED -> IDLE)
 */
static void test_wait_state_lifecycle(void)
{
    struct task_struct task;
    struct nos_object obj;
    struct nos_wait_entry *e;
    struct nos_wake_batch batch;
    int rc;

    init_mock_task(&task, "task_lifecycle", 10);
    e = &task.wait_entry;

    /* 1. 流转: IDLE -> WAITING -> SUCCEEDED -> IDLE */
    nos_object_init(&obj, true);
    nos_wake_batch_init(&batch);

    rc = nos_wait_prepare(e, &obj, 5000ULL);
    assert(rc == 0);
    assert(e->state == NOS_WAIT_STATE_WAITING);
    assert(e->epoch == 1ULL);
    assert(e->deadline == 5000ULL);
    assert(e->waiting_obj == &obj);
    assert(nos_refcount_read(&obj.refcount) == 2U); /* prepare 增加对象引用 */

    /* 在 WAITING 状态下重复 prepare 必须被拒绝 */
    assert(nos_wait_prepare(e, &obj, 6000ULL) == -EBUSY);

    /* 完成等待: SUCCEEDED */
    rc = nos_wait_complete_locked(e, 0, &batch);
    assert(rc == 0);
    assert(e->state == NOS_WAIT_STATE_SUCCEEDED);
    assert(e->result == 0);
    assert(batch.count == 1U);

    /* 结束等待 (finish): 重置为 IDLE 并释放对象引用 */
    nos_wait_finish(e);
    assert(e->state == NOS_WAIT_STATE_IDLE);
    assert(e->waiting_obj == NULL);
    assert(nos_refcount_read(&obj.refcount) == 1U);

    /* 2. 流转: IDLE -> WAITING -> TIMEDOUT -> IDLE */
    nos_wake_batch_init(&batch);
    rc = nos_wait_prepare(e, &obj, 5100ULL);
    assert(rc == 0);
    assert(e->state == NOS_WAIT_STATE_WAITING);
    assert(e->epoch == 2ULL); /* epoch 严格单调递增 */

    rc = nos_wait_complete_locked(e, -ETIMEDOUT, &batch);
    assert(rc == 0);
    assert(e->state == NOS_WAIT_STATE_TIMEDOUT);
    assert(e->result == -ETIMEDOUT);

    nos_wait_finish(e);
    assert(e->state == NOS_WAIT_STATE_IDLE);

    /* 3. 流转: IDLE -> WAITING -> CLOSED -> IDLE */
    nos_wake_batch_init(&batch);
    rc = nos_wait_prepare(e, &obj, 5200ULL);
    assert(rc == 0);
    assert(e->epoch == 3ULL);

    rc = nos_wait_complete_locked(e, -EPIPE, &batch);
    assert(rc == 0);
    assert(e->state == NOS_WAIT_STATE_CLOSED);
    assert(e->result == -EPIPE);

    nos_wait_finish(e);
    assert(e->state == NOS_WAIT_STATE_IDLE);

    /* 4. 流转: IDLE -> WAITING -> CANCELLED -> IDLE */
    nos_wake_batch_init(&batch);
    rc = nos_wait_prepare(e, &obj, 5300ULL);
    assert(rc == 0);
    assert(e->epoch == 4ULL);

    rc = nos_wait_complete_locked(e, -ECANCELED, &batch);
    assert(rc == 0);
    assert(e->state == NOS_WAIT_STATE_CANCELLED);
    assert(e->result == -ECANCELED);

    nos_wait_finish(e);
    assert(e->state == NOS_WAIT_STATE_IDLE);

    /* 5. 对象已关闭时，禁止进入 prepare，必须返回 -EPIPE */
    nos_object_close(&obj);
    assert(nos_wait_prepare(e, &obj, 5400ULL) == -EPIPE);
    assert(e->state == NOS_WAIT_STATE_IDLE);

    puts("[PASS] test_wait_state_lifecycle");
}

/**
 * 测试 3: 单点线性化裁决点 (Single Linearization Point)
 * 一旦某种终态提交，后续竞争调用 (如超时) 必须为 no-op 并返回 -EALREADY
 */
static void test_wait_single_linearization_point(void)
{
    struct task_struct task;
    struct nos_object obj;
    struct nos_wait_entry *e;
    struct nos_wake_batch batch;
    int rc;

    init_mock_task(&task, "task_linear", 5);
    e = &task.wait_entry;
    nos_object_init(&obj, true);
    nos_wake_batch_init(&batch);

    /* Case A: SUCCEEDED 先完成，后来的超时作为 no-op 丢弃 */
    rc = nos_wait_prepare(e, &obj, 3000ULL);
    assert(rc == 0);
    assert(e->state == NOS_WAIT_STATE_WAITING);

    /* 资源交付路径成功提交 */
    rc = nos_wait_complete_locked(e, 0, &batch);
    assert(rc == 0);
    assert(e->state == NOS_WAIT_STATE_SUCCEEDED);
    assert(e->result == 0);
    assert(batch.count == 1U);

    /* 定时器并发到达，试图完成超时 -> 必须被拒绝且不污染状态与批次 */
    rc = nos_wait_complete_locked(e, -ETIMEDOUT, &batch);
    assert(rc == -EALREADY);
    assert(e->state == NOS_WAIT_STATE_SUCCEEDED);
    assert(e->result == 0);
    assert(batch.count == 1U); /* 批次不增加 */

    nos_wait_finish(e);

    /* Case B: TIMEDOUT 先完成，后来的资源交付作为 no-op 丢弃 */
    nos_wake_batch_init(&batch);
    rc = nos_wait_prepare(e, &obj, 4000ULL);
    assert(rc == 0);

    /* 定时器先触发超时 */
    rc = nos_wait_complete_locked(e, -ETIMEDOUT, &batch);
    assert(rc == 0);
    assert(e->state == NOS_WAIT_STATE_TIMEDOUT);
    assert(e->result == -ETIMEDOUT);
    assert(batch.count == 1U);

    /* 原语后续试图交付资源 -> 必须被拒绝 */
    rc = nos_wait_complete_locked(e, 0, &batch);
    assert(rc == -EALREADY);
    assert(e->state == NOS_WAIT_STATE_TIMEDOUT);
    assert(e->result == -ETIMEDOUT);
    assert(batch.count == 1U);

    nos_wait_finish(e);

    puts("[PASS] test_wait_single_linearization_point");
}

/**
 * 测试 4: nos_wait_queue 等待队列操作与实时优先级排序
 */
static void test_wait_queue_priority_order(void)
{
    struct nos_wait_queue wq;
    struct task_struct t_low, t_mid, t_high, t_fifo;
    struct nos_wait_entry *entry;
    struct nos_object obj;

    nos_wait_queue_init(&wq);
    assert(nos_wait_queue_empty(&wq) == true);
    assert(nos_wait_queue_count(&wq) == 0U);

    nos_object_init(&obj, true);
    init_mock_task(&t_low, "low", 20);     /* 优先级 20 (数值大优先级低) */
    init_mock_task(&t_mid, "mid", 10);     /* 优先级 10 */
    init_mock_task(&t_high, "high", 5);    /* 优先级 5 (数值小优先级高) */
    init_mock_task(&t_fifo, "fifo", 10);   /* 同级优先级 10 (同级 FIFO) */

    nos_wait_prepare(&t_low.wait_entry, &obj, 1000ULL);
    nos_wait_prepare(&t_mid.wait_entry, &obj, 1000ULL);
    nos_wait_prepare(&t_high.wait_entry, &obj, 1000ULL);
    nos_wait_prepare(&t_fifo.wait_entry, &obj, 1000ULL);

    /* 按优先级加入队列 */
    nos_wait_queue_add_prio(&wq, &t_low.wait_entry);
    nos_wait_queue_add_prio(&wq, &t_mid.wait_entry);
    nos_wait_queue_add_prio(&wq, &t_high.wait_entry);
    nos_wait_queue_add_prio(&wq, &t_fifo.wait_entry);

    assert(nos_wait_queue_count(&wq) == 4U);
    assert(!nos_wait_queue_empty(&wq));

    /* 校验队首及出队顺序: high (5) -> mid (10) -> fifo (10) -> low (20) */
    entry = nos_wait_queue_first(&wq);
    assert(entry == &t_high.wait_entry);
    nos_wait_queue_del(&wq, entry);

    entry = nos_wait_queue_first(&wq);
    assert(entry == &t_mid.wait_entry);
    nos_wait_queue_del(&wq, entry);

    entry = nos_wait_queue_first(&wq);
    assert(entry == &t_fifo.wait_entry);
    nos_wait_queue_del(&wq, entry);

    entry = nos_wait_queue_first(&wq);
    assert(entry == &t_low.wait_entry);
    nos_wait_queue_del(&wq, entry);

    assert(nos_wait_queue_empty(&wq));
    assert(nos_wait_queue_count(&wq) == 0U);

    nos_wait_finish(&t_low.wait_entry);
    nos_wait_finish(&t_mid.wait_entry);
    nos_wait_finish(&t_high.wait_entry);
    nos_wait_finish(&t_fifo.wait_entry);

    puts("[PASS] test_wait_queue_priority_order");
}

/**
 * 测试 5: 延后唤醒批次 nos_wake_batch 与单向锁序验证
 * 必须在 object->lock 外进行 dispatch，并在任务锁保护下唤醒任务
 */
static void test_wake_batch_deferred_dispatch(void)
{
    struct task_struct t1, t2, t3;
    struct nos_object obj;
    struct nos_wake_batch batch;
    spinlock_t mock_obj_lock = 0;

    init_mock_task(&t1, "t1", 1);
    init_mock_task(&t2, "t2", 2);
    init_mock_task(&t3, "t3", 3);

    nos_object_init(&obj, true);
    nos_wake_batch_init(&batch);

    nos_wait_prepare(&t1.wait_entry, &obj, 1000ULL);
    nos_wait_prepare(&t2.wait_entry, &obj, 1000ULL);
    nos_wait_prepare(&t3.wait_entry, &obj, 1000ULL);

    /* 模拟持有 object->lock 完成等待者裁决并加入唤醒批次 */
    mock_obj_lock = 1;
    s_monitored_obj_lock = &mock_obj_lock;
    s_object_lock_held_during_dispatch = false;
    s_wakeup_count = 0;

    nos_wait_complete_locked(&t1.wait_entry, 0, &batch);
    nos_wait_complete_locked(&t2.wait_entry, 0, &batch);
    nos_wait_complete_locked(&t3.wait_entry, 0, &batch);

    assert(batch.count == 3U);
    assert(s_wakeup_count == 0); /* 批次收集阶段不发生唤醒调度 */

    /* 关键架构约束：必须先释放 object->lock，再执行 wake_batch_dispatch */
    mock_obj_lock = 0;

    nos_wake_batch_dispatch(&batch);

    /* 校验全部 3 个任务被安全唤醒 */
    assert(s_wakeup_count == 3);
    assert(s_object_lock_held_during_dispatch == false);
    assert(batch.count == 0U);
    assert(list_empty(&batch.list));

    assert(t1.status == 0x02); /* TASK_READY */
    assert(t2.status == 0x02);
    assert(t3.status == 0x02);

    nos_wait_finish(&t1.wait_entry);
    nos_wait_finish(&t2.wait_entry);
    nos_wait_finish(&t3.wait_entry);

    s_monitored_obj_lock = NULL;

    puts("[PASS] test_wake_batch_deferred_dispatch");
}

/**
 * 测试 6: task_struct 内嵌 refcount 引用计数操作
 */
static void test_task_refcount(void)
{
    struct task_struct task;

    init_mock_task(&task, "task_ref", 10);
    assert(nos_refcount_read(&task.refcount) == 1U);

    assert(task_get(&task) == true);
    assert(nos_refcount_read(&task.refcount) == 2U);

    assert(task_put(&task) == false);
    assert(nos_refcount_read(&task.refcount) == 1U);

    assert(task_put(&task) == true);
    assert(nos_refcount_read(&task.refcount) == 0U);

    puts("[PASS] test_task_refcount");
}

int main(void)
{
    puts("=== Running Universal Wait Foundation Tests ===");
    test_wait_entry_init_and_params();
    test_wait_state_lifecycle();
    test_wait_single_linearization_point();
    test_wait_queue_priority_order();
    test_wake_batch_deferred_dispatch();
    test_task_refcount();
    puts("All universal wait tests PASS");
    return 0;
}
