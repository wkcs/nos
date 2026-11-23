/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * 宿主端 TDD 测试：AI 推理感知调度器 sched_infer
 * 覆盖：RT > INFER > FAIR 调度链、EDF 截止期排序与抢占、CBS 周期节流与解禁、
 * 截止期继承 (Deadline Inheritance)、异构核 EAS 选核与 Gang 多核同步唤醒。
 */

#include <nos/sched.h>
#include <nos/sched_infer.h>
#include <kernel/sched.h>
#include <kernel/task.h>
#include <kernel/cpu.h>
#include <nos/hal_arch.h>
#include <uapi/nos/errno.h>
#include <uapi/nos/ai.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

addr_t kernel_running = 1U;
static uint32_t g_sim_cpu_id;
static uint64_t g_sim_time_us = 1000ULL;
static uint32_t g_ipi_count[CONFIG_NR_CPUS];
static struct task_struct g_idle_tasks[CONFIG_NR_CPUS];
static uint32_t g_oneshot_calls;
static uint64_t g_last_oneshot_ns;

void hal_timer_program_oneshot_ns(uint64_t delta_ns)
{
    g_oneshot_calls++;
    g_last_oneshot_ns = delta_ns;
}

/* Mock HAL & Kernel 基础依赖 */
uint32_t hal_cpu_id(void)
{
    return g_sim_cpu_id;
}

void hal_smp_send_ipi(uint32_t target_cpu, uint32_t ipi_type)
{
    (void)ipi_type;
    if (target_cpu < CONFIG_NR_CPUS) {
        g_ipi_count[target_cpu]++;
    }
}

void hal_vm_switch(struct nos_vm_space *next_space)
{
    (void)next_space;
}

uint64_t hal_timer_monotonic_ns(void)
{
    return g_sim_time_us * 1000ULL;
}

u64 cpu_run_time_us(void)
{
    return g_sim_time_us;
}

void context_switch(addr_t from, addr_t to)
{
    (void)from;
    (void)to;
}

void context_switch_to(addr_t to)
{
    (void)to;
}

addr_t disable_irq_save(void)
{
    return 0U;
}

void enable_irq_save(addr_t level)
{
    (void)level;
}

int pr_log(bool no_tag, enum log_level level, const char *fmt, ...)
{
    (void)no_tag;
    (void)level;
    (void)fmt;
    return 0;
}

int nos_cell_charge_cpu(struct nos_cell *cell, uint64_t delta_ns)
{
    (void)cell;
    (void)delta_ns;
    return 0;
}

struct nos_vm_space *nos_cell_vm_space(struct nos_cell *cell)
{
    (void)cell;
    return NULL;
}

struct nos_cap_table *nos_cell_cap_table(struct nos_cell *cell)
{
    (void)cell;
    return NULL;
}

void idel_task_init(void)
{
    uint32_t cpu;
    for (cpu = 0U; cpu < CONFIG_NR_CPUS; cpu++) {
        struct task_struct *idle = &g_idle_tasks[cpu];
        memset(idle, 0, sizeof(*idle));
        idle->name = "idle";
        idle->policy = SCHED_POLICY_IDLE;
        idle->current_priority = CONFIG_MAX_PRIORITY - 1U;
        idle->cpu = (uint8_t)cpu;
        idle->cpus_allowed = (uint8_t)(1U << cpu);
        idle->sched_class = &idle_sched_class;
        INIT_LIST_HEAD(&idle->se.run_node);
        nos_refcount_init(&idle->refcount, 1U);
        g_sched_rqs[cpu].idle = idle;
        g_sched_rqs[cpu].curr = idle;
    }
}

/* 覆盖 weak 拓扑函数：模拟 CPU 0..1 为小核，CPU 2..3 为带 I8MM/SVE 大核 */
int arch_sched_query_topo(struct nos_cpu_topo_info *out_info)
{
    memset(out_info, 0, sizeof(*out_info));
    out_info->nr_cpus = CONFIG_NR_CPUS;
    out_info->online_mask = 0x0FU;
    out_info->little_cpu_mask = 0x03U; /* CPU 0, 1 */
    out_info->big_cpu_mask = 0x0CU;    /* CPU 2, 3 */
    out_info->ai_isa_flags = NOS_AI_ISA_DOTPROD | NOS_AI_ISA_I8MM | NOS_AI_ISA_SVE;
    return 0;
}

static void init_mock_task(struct task_struct *t, const char *name, pid_t pid,
                           uint8_t policy, uint8_t prio, uint32_t cpu)
{
    memset(t, 0, sizeof(*t));
    INIT_LIST_HEAD(&t->se.run_node);
    INIT_LIST_HEAD(&t->list);
    INIT_LIST_HEAD(&t->tlist);
    INIT_LIST_HEAD(&t->held_mutex_list);
    spin_lock_init(&t->lock);
    nos_refcount_init(&t->refcount, 1U);
    t->name = name;
    t->pid = pid;
    t->policy = policy;
    t->init_priority = prio;
    t->current_priority = prio;
    t->cpu = (uint8_t)cpu;
    t->cpus_allowed = NOS_CPU_MASK_ALL;
    t->status = TASK_SUSPENDED;
    t->init_tick = 10U;
    t->remaining_tick = 10U;
    t->sched_class = sched_policy_to_class(policy);
    sched_load_task_init(t, 0);
}

static void test_edf_cbs_and_class_hierarchy(void)
{
    struct task_struct rt_task;
    struct task_struct t_early;
    struct task_struct t_mid;
    struct task_struct t_late;
    struct task_struct fair_task;
    struct nos_infer_sched_param p_early = {
        .role = NOS_INFER_ROLE_DECODE,
        .gang_id = 0U,
        .runtime_ns = 2000000ULL,   /* 2 ms */
        .deadline_ns = 5000000ULL,  /* 5 ms */
        .period_ns = 10000000ULL,   /* 10 ms */
    };
    struct nos_infer_sched_param p_mid = {
        .role = NOS_INFER_ROLE_DECODE,
        .gang_id = 0U,
        .runtime_ns = 3000000ULL,
        .deadline_ns = 12000000ULL, /* 12 ms */
        .period_ns = 20000000ULL,
    };
    struct nos_infer_sched_param p_late = {
        .role = NOS_INFER_ROLE_PREFILL,
        .gang_id = 0U,
        .runtime_ns = 4000000ULL,
        .deadline_ns = 20000000ULL, /* 20 ms */
        .period_ns = 30000000ULL,
    };
    struct sched_rq *rq;

    g_sim_cpu_id = 0U;
    g_sim_time_us = 1000000ULL;
    sch_init();
    rq = sched_rq_of(0U);

    init_mock_task(&rt_task, "rt", 10, SCHED_POLICY_RR, 2U, 0U);
    init_mock_task(&fair_task, "fair", 11, SCHED_POLICY_NORMAL, 10U, 0U);
    init_mock_task(&t_late, "inf_late", 12, SCHED_POLICY_NORMAL, 10U, 0U);
    init_mock_task(&t_mid, "inf_mid", 13, SCHED_POLICY_NORMAL, 10U, 0U);
    init_mock_task(&t_early, "inf_early", 14, SCHED_POLICY_NORMAL, 10U, 0U);

    assert(sched_infer_set_param(&t_late, &p_late) == 0);
    assert(sched_infer_set_param(&t_mid, &p_mid) == 0);
    assert(sched_infer_set_param(&t_early, &p_early) == 0);

    /* 1. 将 Fair 与 3 个 INFER 任务入到 CPU 0 上验证优先级与 EDF 顺序 */
    spin_lock_irq(&rq->lock);
    sched_enqueue_task_locked(rq, &fair_task, SCHED_ENQUEUE_WAKEUP);
    sched_enqueue_task_locked(rq, &t_late, SCHED_ENQUEUE_WAKEUP);
    rq->curr = &t_late;
    t_late.status = TASK_RUNNING;
    rq->need_resched = false;

    /* 新到达更早截止期 t_early 必须立即触发 EDF 抢占标记 */
    sched_enqueue_task_locked(rq, &t_mid, SCHED_ENQUEUE_WAKEUP);
    sched_enqueue_task_locked(rq, &t_early, SCHED_ENQUEUE_WAKEUP);
    assert(rq->need_resched == true);

    assert(infer_sched_class.pick_next_task(rq) == &t_early);

    /* 2. CBS 节流检验：t_early 仅 2ms runtime，推进 2 次 1ms tick 后必须进入节流，退让给 t_mid */
    rq->curr = &t_early;
    g_sim_time_us += 1000ULL;
    infer_sched_class.task_tick(rq, &t_early);
    assert(!t_early.se.infer.throttled);
    g_sim_time_us += 1000ULL;
    infer_sched_class.task_tick(rq, &t_early);
    assert(t_early.se.infer.throttled);
    assert(infer_sched_class.pick_next_task(rq) == &t_mid);

    /* 同样消耗 t_mid 和 t_late 全部配额后，infer_sched_class 必须返回 NULL 让 fair 接管 */
    t_mid.se.infer.throttled = true;
    t_mid.se.infer.throttle_until_ns = hal_timer_monotonic_ns() + 20000000ULL;
    t_late.se.infer.throttled = true;
    t_late.se.infer.throttle_until_ns = hal_timer_monotonic_ns() + 20000000ULL;
    assert(infer_sched_class.pick_next_task(rq) == NULL);

    /* RT 任务压过 INFER，但一旦 RT 出队且周期越过 10ms，t_early 自动解禁补给恢复 */
    sched_enqueue_task_locked(rq, &rt_task, SCHED_ENQUEUE_WAKEUP);
    assert(rt_sched_class.pick_next_task(rq) == &rt_task);
    sched_dequeue_task_locked(rq, &rt_task, SCHED_DEQUEUE_SLEEP);

    g_sim_time_us += 10000ULL; /* 越过 t_early 的 10ms 周期 */
    assert(infer_sched_class.pick_next_task(rq) == &t_early);
    assert(!t_early.se.infer.throttled);
    assert(t_early.se.infer.remaining_runtime_ns == p_early.runtime_ns);
    spin_unlock_irq(&rq->lock);
}

static void test_deadline_inheritance_eas_and_gang(void)
{
    struct task_struct t_prefill;
    struct task_struct t_ctrl;
    struct task_struct t_worker;
    struct task_struct g_m0;
    struct task_struct g_m1;
    struct nos_infer_sched_param p_prefill = {
        .role = NOS_INFER_ROLE_PREFILL,
        .gang_id = 0U,
        .runtime_ns = 4000000ULL,
        .deadline_ns = 15000000ULL,
        .period_ns = 20000000ULL,
    };
    struct nos_infer_sched_param p_ctrl = {
        .role = NOS_INFER_ROLE_AGENT_CTRL,
        .gang_id = 0U,
        .runtime_ns = 1000000ULL,
        .deadline_ns = 8000000ULL,
        .period_ns = 10000000ULL,
    };
    struct nos_cpu_topo_info topo;
    struct sched_rq *rq0;
    uint32_t picked_cpu;
    uint64_t urgent_dl;

    g_sim_cpu_id = 0U;
    g_sim_time_us = 2000000ULL;
    memset(g_ipi_count, 0, sizeof(g_ipi_count));
    sch_init();
    rq0 = sched_rq_of(0U);

    init_mock_task(&t_prefill, "prefill", 20, SCHED_POLICY_NORMAL, 10U, 0U);
    init_mock_task(&t_ctrl, "ctrl", 21, SCHED_POLICY_NORMAL, 10U, 2U);
    init_mock_task(&t_worker, "worker", 22, SCHED_POLICY_NORMAL, 12U, 0U);

    assert(sched_infer_set_param(&t_prefill, &p_prefill) == 0);
    assert(sched_infer_set_param(&t_ctrl, &p_ctrl) == 0);

    /* 1. 验证 EAS 异构选核：PREFILL->大核 (2/3)，AGENT_CTRL->小核 (0/1) */
    picked_cpu = sched_select_task_rq(&t_prefill, SCHED_ENQUEUE_WAKEUP);
    assert(picked_cpu == 2U || picked_cpu == 3U);

    picked_cpu = sched_select_task_rq(&t_ctrl, SCHED_ENQUEUE_WAKEUP);
    assert(picked_cpu == 0U || picked_cpu == 1U);

    /* 2. 截止期继承：将普通 Fair worker 紧急提升入 INFER 队列头，完成再精确还原为 NORMAL */
    spin_lock_irq(&rq0->lock);
    sched_enqueue_task_locked(rq0, &t_ctrl, SCHED_ENQUEUE_WAKEUP);
    sched_enqueue_task_locked(rq0, &t_worker, SCHED_ENQUEUE_WAKEUP);
    spin_unlock_irq(&rq0->lock);

    assert(infer_sched_class.pick_next_task(rq0) == &t_ctrl);
    urgent_dl = hal_timer_monotonic_ns() + 500000ULL; /* 0.5 ms 极紧急截止期 */
    assert(sched_infer_boost_deadline(&t_worker, urgent_dl) == 0);
    assert(t_worker.policy == SCHED_POLICY_INFER);
    assert(t_worker.se.infer.deadline_inherited);
    assert(infer_sched_class.pick_next_task(rq0) == &t_worker);

    sched_infer_restore_deadline(&t_worker);
    assert(t_worker.policy == SCHED_POLICY_NORMAL);
    assert(!t_worker.se.infer.deadline_inherited);
    assert(infer_sched_class.pick_next_task(rq0) == &t_ctrl);

    /* 3. Gang 协同唤醒跨核 IPI 联动 + 拓扑活跃 Gang 统计 */
    init_mock_task(&g_m0, "gang0", 30, SCHED_POLICY_INFER, 5U, 1U);
    init_mock_task(&g_m1, "gang1", 31, SCHED_POLICY_INFER, 5U, 2U);
    assert(sched_infer_gang_join(&g_m0, 7U) == 0);
    assert(sched_infer_gang_join(&g_m1, 7U) == 0);
    assert(sched_infer_query_topo(&topo) == 0);
    assert(topo.active_gangs == 1U);
    assert(topo.ai_isa_flags == (NOS_AI_ISA_DOTPROD | NOS_AI_ISA_I8MM | NOS_AI_ISA_SVE));

    assert(sched_infer_gang_wakeup(7U) == 2);
    assert(g_m0.se.on_rq && g_m1.se.on_rq);
    assert(g_ipi_count[1] == 1U && g_ipi_count[2] == 1U);

    sched_infer_gang_leave(&g_m0);
    sched_infer_gang_leave(&g_m1);
    assert(sched_infer_query_topo(&topo) == 0);
    assert(topo.active_gangs == 0U);

    /* 4. DECODE 访存核簇打散惩罚与微秒级 oneshot_ns 精准抢占验证 */
    struct task_struct dec0;
    struct task_struct dec1;
    struct nos_infer_sched_param p_dec_micro = {
        .role = NOS_INFER_ROLE_DECODE,
        .gang_id = 0U,
        .runtime_ns = 250000ULL, /* 250 us (< 1ms tick) */
        .deadline_ns = 5000000ULL,
        .period_ns = 10000000ULL,
    };
    struct sched_rq *rq1 = sched_rq_of(1U);

    init_mock_task(&dec0, "dec0", 40, SCHED_POLICY_NORMAL, 10U, 0U);
    init_mock_task(&dec1, "dec1", 41, SCHED_POLICY_NORMAL, 10U, 0U);
    assert(sched_infer_set_param(&dec0, &p_dec_micro) == 0);
    assert(sched_infer_set_param(&dec1, &p_dec_micro) == 0);

    spin_lock_irq(&rq0->lock);
    sched_enqueue_task_locked(rq0, &dec0, SCHED_ENQUEUE_WAKEUP);
    rq0->curr = &dec0;
    dec0.status = TASK_RUNNING;
    spin_unlock_irq(&rq0->lock);

    /* dec1 选核应避开正在跑 DECODE 的 cpu0 而分散到空闲核 */
    picked_cpu = sched_select_task_rq(&dec1, SCHED_ENQUEUE_WAKEUP);
    assert(picked_cpu != 0U && picked_cpu < CONFIG_NR_CPUS);

    /* pick_next_task 对 remaining < 1ms 触发 oneshot_ns(250us)，推进 250us 即刻精准节流 */
    g_oneshot_calls = 0U;
    g_last_oneshot_ns = 0ULL;
    spin_lock_irq(&rq1->lock);
    sched_enqueue_task_locked(rq1, &dec1, SCHED_ENQUEUE_WAKEUP);
    assert(infer_sched_class.pick_next_task(rq1) == &dec1);
    assert(g_oneshot_calls == 1U && g_last_oneshot_ns == 250000ULL);
    rq1->curr = &dec1;
    g_sim_time_us += 250ULL;
    infer_sched_class.task_tick(rq1, &dec1);
    assert(dec1.se.infer.throttled);
    spin_unlock_irq(&rq1->lock);
}

int main(void)
{
    test_edf_cbs_and_class_hierarchy();
    test_deadline_inheritance_eas_and_gang();
    printf("PASS: test_sched_infer (EDF, CBS throttle/replenish, Deadline Inheritance, EAS, Gang)\n");
    return 0;
}
