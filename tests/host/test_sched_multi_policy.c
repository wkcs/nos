/**
 * Copyright (C) 2026 胡启航<Nick Hu>
 *
 * Host unit tests for NOS multi-policy scheduler (RT, Fair, Idle),
 * Per-CPU SMP runqueues, deferred rescheduling, and PELT / LoadAvg.
 */

#include <nos/sched.h>
#include <kernel/sched.h>
#include <kernel/task.h>
#include <kernel/mutex.h>
#include <kernel/cpu.h>
#include <nos/hal_arch.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Simulated kernel & HAL state */
addr_t kernel_running = 1U;
static uint32_t g_sim_cpu_id;
static uint64_t g_sim_time_us;
static uint32_t g_ipi_count[CONFIG_NR_CPUS];
static uint32_t g_last_ipi_type[CONFIG_NR_CPUS];
static uint32_t g_ctx_switch_calls;
static struct task_struct g_idle_tasks[CONFIG_NR_CPUS];

uint32_t hal_cpu_id(void)
{
    return g_sim_cpu_id;
}

void hal_smp_send_ipi(uint32_t target_cpu, uint32_t ipi_type)
{
    if (target_cpu < CONFIG_NR_CPUS) {
        g_ipi_count[target_cpu]++;
        g_last_ipi_type[target_cpu] = ipi_type;
    }
}

void hal_vm_switch(struct nos_vm_space *next_space)
{
    (void)next_space;
}

u64 cpu_run_time_us(void)
{
    return g_sim_time_us;
}

void context_switch(addr_t from, addr_t to)
{
    (void)from;
    (void)to;
    g_ctx_switch_calls++;
}

void context_switch_to(addr_t to)
{
    (void)to;
    g_ctx_switch_calls++;
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

void panic(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    abort();
}

static void init_mock_task(struct task_struct *t, const char *name, pid_t pid,
                           uint8_t policy, uint8_t prio, int8_t nice, uint32_t tick)
{
    memset(t, 0, sizeof(*t));
    t->name = name;
    t->pid = pid;
    t->policy = policy;
    t->init_priority = prio;
    t->current_priority = prio;
    t->pi_boost_prio = TASK_PRIO_INVALID;
    t->nice = nice;
    t->init_tick = tick;
    t->remaining_tick = tick;
    t->cpus_allowed = 0x01U;
    t->status = TASK_SUSPENDED;
    t->sched_class = sched_policy_to_class(policy);
    INIT_LIST_HEAD(&t->list);
    INIT_LIST_HEAD(&t->tlist);
    INIT_LIST_HEAD(&t->held_mutex_list);
    spin_lock_init(&t->lock);
    sched_load_task_init(t, nice);
}

static void mock_apply_prio_locked(struct task_struct *task, uint8_t eff_prio)
{
    if (task->current_priority == eff_prio) {
        return;
    }
    if (task->se.on_rq) {
        del_task_to_ready_list_lock(task);
        task->current_priority = eff_prio;
        add_task_to_ready_list_lock(task);
    } else {
        task->current_priority = eff_prio;
    }
}

int task_set_prio(struct task_struct *task, uint8_t prio)
{
    uint8_t eff_prio;

    spin_lock_irq(&task->lock);
    task->init_priority = prio;
    eff_prio = (task->pi_boost_prio < task->init_priority) ?
               task->pi_boost_prio : task->init_priority;
    mock_apply_prio_locked(task, eff_prio);
    spin_unlock_irq(&task->lock);
    return 0;
}

int task_pi_boost(struct task_struct *owner, uint8_t waiter_prio)
{
    uint8_t eff_prio;

    spin_lock_irq(&owner->lock);
    if (waiter_prio < owner->pi_boost_prio) {
        owner->pi_boost_prio = waiter_prio;
    }
    eff_prio = (owner->pi_boost_prio < owner->init_priority) ?
               owner->pi_boost_prio : owner->init_priority;
    mock_apply_prio_locked(owner, eff_prio);
    spin_unlock_irq(&owner->lock);
    return 0;
}

int task_pi_unboost(struct task_struct *owner, uint8_t next_waiter_prio)
{
    uint8_t best_boost = next_waiter_prio;
    uint8_t eff_prio;
    struct mutex *m;

    spin_lock_irq(&owner->lock);
    list_for_each_entry(m, &owner->held_mutex_list, held_node) {
        if (!list_empty(&m->wait_list)) {
            struct task_struct *w =
                list_first_entry(&m->wait_list, struct task_struct, list);
            if (w->current_priority < best_boost) {
                best_boost = w->current_priority;
            }
        }
    }
    owner->pi_boost_prio = best_boost;
    eff_prio = (owner->pi_boost_prio < owner->init_priority) ?
               owner->pi_boost_prio : owner->init_priority;
    mock_apply_prio_locked(owner, eff_prio);
    spin_unlock_irq(&owner->lock);
    return 0;
}

int idel_task_init(void)
{
    uint32_t i;

    for (i = 0U; i < CONFIG_NR_CPUS; i++) {
        init_mock_task(&g_idle_tasks[i], "idel", (pid_t)(i + 1U),
                       SCHED_POLICY_IDLE, IDEL_TASK_PRIO, 0, 10U);
        g_idle_tasks[i].cpu = (uint8_t)i;
        g_idle_tasks[i].cpus_allowed = (uint8_t)(1U << i);
        g_sched_rqs[i].idle = &g_idle_tasks[i];
        sched_enqueue_task_locked(&g_sched_rqs[i], &g_idle_tasks[i], 0);
    }
    return 0;
}

static void reset_test_env(void)
{
    g_sim_cpu_id = 0U;
    g_sim_time_us = 1000ULL;
    g_ctx_switch_calls = 0U;
    memset(g_ipi_count, 0, sizeof(g_ipi_count));
    memset(g_last_ipi_type, 0, sizeof(g_last_ipi_type));
    kernel_running = 1U;
    sch_init();
}

static void test_rt_preemption_fifo_and_rr(void)
{
    struct task_struct t_low, t_high, t_fifo1, t_fifo2, t_rr1, t_rr2;
    struct sched_rq *rq;

    reset_test_env();
    rq = this_rq();

    /* 1. Start on idle when no RT/Fair tasks are runnable */
    switch_task();
    assert(task_current() == &g_idle_tasks[0]);
    assert(rq->nr_running == 0U);

    /* 2. Enqueue lower-priority RT task (prio 5) -> preempts idle */
    init_mock_task(&t_low, "rt_low", 10, SCHED_POLICY_RR, 5, 0, 3);
    add_task_to_ready_list(&t_low);
    assert(rq->need_resched);
    switch_task();
    assert(task_current() == &t_low);
    assert(t_low.status == TASK_RUNNING);

    /* 3. Enqueue higher-priority RT task (prio 2) -> preempts prio 5 */
    init_mock_task(&t_high, "rt_high", 11, SCHED_POLICY_RR, 2, 0, 3);
    add_task_to_ready_list(&t_high);
    assert(rq->need_resched);
    switch_task();
    assert(task_current() == &t_high);
    assert(t_low.status == TASK_READY);
    assert(t_low.se.nivcsw == 1U);

    /* Dequeue both to test FIFO vs RR */
    del_task_to_ready_list(&t_high);
    del_task_to_ready_list(&t_low);
    switch_task();
    assert(task_current() == &g_idle_tasks[0]);

    /* 4. SCHED_POLICY_FIFO: no timeslice rotation across ticks */
    init_mock_task(&t_fifo1, "fifo1", 12, SCHED_POLICY_FIFO, 3, 0, 2);
    init_mock_task(&t_fifo2, "fifo2", 13, SCHED_POLICY_FIFO, 3, 0, 2);
    add_task_to_ready_list(&t_fifo1);
    add_task_to_ready_list(&t_fifo2);
    switch_task();
    assert(task_current() == &t_fifo1);

    for (int i = 0; i < 6; i++) {
        g_sim_time_us += 1000ULL;
        sch_heartbeat();
        assert(task_current() == &t_fifo1);
    }

    /* Explicit yield rotates FIFO to fifo2 */
    rt_sched_class.yield_task(rq, &t_fifo1);
    switch_task();
    assert(task_current() == &t_fifo2);

    del_task_to_ready_list(&t_fifo1);
    del_task_to_ready_list(&t_fifo2);

    /* 5. SCHED_POLICY_RR: rotates every init_tick (2 ticks) */
    init_mock_task(&t_rr1, "rr1", 14, SCHED_POLICY_RR, 4, 0, 2);
    init_mock_task(&t_rr2, "rr2", 15, SCHED_POLICY_RR, 4, 0, 2);
    add_task_to_ready_list(&t_rr1);
    add_task_to_ready_list(&t_rr2);
    switch_task();
    assert(task_current() == &t_rr1);

    g_sim_time_us += 1000ULL;
    sch_heartbeat();
    assert(task_current() == &t_rr1);

    g_sim_time_us += 1000ULL;
    sch_heartbeat();
    assert(task_current() == &t_rr2);

    g_sim_time_us += 1000ULL;
    sch_heartbeat();
    assert(task_current() == &t_rr2);

    g_sim_time_us += 1000ULL;
    sch_heartbeat();
    assert(task_current() == &t_rr1);

    del_task_to_ready_list(&t_rr1);
    del_task_to_ready_list(&t_rr2);
    switch_task();
    assert(task_current() == &g_idle_tasks[0]);
}

static void test_fair_vruntime_weights_and_rt_preempt(void)
{
    struct task_struct t_norm0, t_norm_neg5, t_sleeper, t_rt;
    uint64_t ratio_x100;

    reset_test_env();

    assert(sched_nice_to_weight(0) == 1024U);
    assert(sched_nice_to_weight(-5) == 3121U);
    assert(sched_nice_to_weight(-10) == 9548U);
    assert(sched_nice_to_weight(5) == 335U);
    assert(sched_nice_to_weight(19) == 15U);

    init_mock_task(&t_norm0, "fair_n0", 20, SCHED_POLICY_NORMAL, 16, 0, 10);
    init_mock_task(&t_norm_neg5, "fair_n_5", 21, SCHED_POLICY_NORMAL, 16, -5, 10);
    add_task_to_ready_list(&t_norm0);
    add_task_to_ready_list(&t_norm_neg5);
    switch_task();

    /* Run 200 ticks (1ms each) and verify nice -5 gets ~3x CPU time of nice 0 */
    for (int i = 0; i < 200; i++) {
        g_sim_time_us += 1000ULL;
        sch_heartbeat();
    }

    assert(t_norm_neg5.se.sum_exec_us > t_norm0.se.sum_exec_us * 2ULL);
    ratio_x100 = (t_norm_neg5.se.sum_exec_us * 100ULL) / t_norm0.se.sum_exec_us;
    assert(ratio_x100 >= 250ULL && ratio_x100 <= 360ULL);

    /* Sleeper wakeup compensation clamps vruntime to min_vruntime - SCHED_LATENCY_US/2 */
    init_mock_task(&t_sleeper, "fair_sleep", 22, SCHED_POLICY_NORMAL, 16, 0, 10);
    t_sleeper.se.sum_exec_us = 1000ULL;
    t_sleeper.se.vruntime = 100ULL;
    add_task_to_ready_list(&t_sleeper);
    assert(t_sleeper.se.vruntime >= this_rq()->fair.min_vruntime - (SCHED_LATENCY_US / 2ULL));
    switch_task();
    assert(task_current() == &t_sleeper);

    /* RT task always preempts running Fair task */
    init_mock_task(&t_rt, "rt_urgent", 23, SCHED_POLICY_FIFO, 10, 0, 5);
    add_task_to_ready_list(&t_rt);
    assert(this_rq()->need_resched);
    switch_task();
    assert(task_current() == &t_rt);

    del_task_to_ready_list(&t_rt);
    del_task_to_ready_list(&t_sleeper);
    del_task_to_ready_list(&t_norm0);
    del_task_to_ready_list(&t_norm_neg5);
    switch_task();
    assert(task_current() == &g_idle_tasks[0]);
}

static void test_deferred_resched_under_sch_lock(void)
{
    struct task_struct t1, t2;
    struct sched_rq *rq;

    reset_test_env();
    rq = this_rq();

    init_mock_task(&t1, "t1", 30, SCHED_POLICY_RR, 5, 0, 5);
    init_mock_task(&t2, "t2", 31, SCHED_POLICY_RR, 1, 0, 5);

    add_task_to_ready_list(&t1);
    switch_task();
    assert(task_current() == &t1);

    sch_lock();
    assert(get_sch_lock_level() == 1U);
    add_task_to_ready_list(&t2);
    /* Attempting to switch while scheduler is locked must defer via need_resched */
    switch_task();
    assert(task_current() == &t1);
    assert(rq->need_resched);

    sch_unlock();
    assert(get_sch_lock_level() == 0U);
    assert(rq->need_resched);
    sched_resched_check();
    assert(task_current() == &t2);
    assert(!rq->need_resched);
}

static void test_smp_select_rq_ipi_and_work_stealing(void)
{
    struct task_struct t_cpu0_run, t_cpu0_wait, t_pinned2, t_auto;
    int moved;

    reset_test_env();
    g_sim_cpu_id = 0U;

    /* Put 2 tasks on CPU 0 (t_cpu0_run running, t_cpu0_wait ready) */
    init_mock_task(&t_cpu0_run, "c0_run", 40, SCHED_POLICY_NORMAL, 16, 0, 5);
    init_mock_task(&t_cpu0_wait, "c0_wait", 41, SCHED_POLICY_NORMAL, 16, 0, 5);
    t_cpu0_run.cpus_allowed = 0x01U; /* pinned to CPU 0 */
    t_cpu0_wait.cpus_allowed = 0x0FU; /* allowed on CPUs 0..3 */

    sched_enqueue_task_locked(&g_sched_rqs[0], &t_cpu0_run, SCHED_ENQUEUE_WAKEUP);
    switch_task();
    assert(g_sched_rqs[0].curr == &t_cpu0_run);
    assert(t_cpu0_run.on_cpu == 1U);

    sched_enqueue_task_locked(&g_sched_rqs[0], &t_cpu0_wait, SCHED_ENQUEUE_WAKEUP);
    assert(g_sched_rqs[0].nr_running == 2U);

    /* Task pinned to CPU 2 must select CPU 2 and trigger IPI from CPU 0 */
    init_mock_task(&t_pinned2, "pin2", 42, SCHED_POLICY_RR, 3, 0, 5);
    t_pinned2.cpus_allowed = (1U << 2);
    assert(sched_select_task_rq(&t_pinned2, SCHED_ENQUEUE_WAKEUP) == 2U);
    add_task_to_ready_list(&t_pinned2);
    assert(t_pinned2.cpu == 2U);
    assert(g_sched_rqs[2].nr_running == 1U);
    assert(g_ipi_count[2] == 1U);
    assert(g_last_ipi_type[2] == NOS_IPI_RESCHED);

    /* Auto-placed task with prev_cpu=0 (busy) should pick idle CPU 1 */
    init_mock_task(&t_auto, "auto1", 43, SCHED_POLICY_NORMAL, 16, 0, 5);
    t_auto.cpu = 0U;
    t_auto.cpus_allowed = 0x0FU;
    assert(sched_select_task_rq(&t_auto, SCHED_ENQUEUE_WAKEUP) == 1U);

    /* Idle CPU 3 steals waiting task t_cpu0_wait from overloaded CPU 0 */
    assert(g_sched_rqs[3].nr_running == 0U);
    moved = sched_balance_rq(3U);
    assert(moved == 1);
    assert(t_cpu0_wait.cpu == 3U);
    assert(g_sched_rqs[0].nr_running == 1U);
    assert(g_sched_rqs[3].nr_running == 1U);

    /* Switch on CPU 3 runs stolen task t_cpu0_wait */
    g_sim_cpu_id = 3U;
    switch_task();
    assert(g_sched_rqs[3].curr == &t_cpu0_wait);
    assert(t_cpu0_wait.on_cpu == 1U);
}

static void test_pelt_and_loadavg(void)
{
    struct task_struct t_busy;
    uint32_t loads[3] = { 0 };
    uint32_t nr_run = 0U, nr_tot = 0U;
    struct sched_rq *rq;

    reset_test_env();
    rq = this_rq();
    init_mock_task(&t_busy, "busy", 50, SCHED_POLICY_RR, 2, 0, 10);
    add_task_to_ready_list(&t_busy);
    switch_task();

    /* Run 100ms (1 full PELT window) active */
    g_sim_time_us += SCHED_PELT_WINDOW_US;
    sched_load_update_task(rq, &t_busy, g_sim_time_us, SCHED_PELT_WINDOW_US, false);
    assert(sched_load_task_util(&t_busy, g_sim_time_us) == SCHED_CAPACITY_SCALE);
    assert(get_cpu_usage() == 10000U);

    /* Decay across 1 idle window (100ms): 1024 * 3 / 4 = 768 */
    g_sim_time_us += SCHED_PELT_WINDOW_US;
    sched_load_decay_task(&t_busy, g_sim_time_us);
    assert(sched_load_task_util(&t_busy, g_sim_time_us) == 768U);

    /* Advance 5s and update system loadavg */
    g_sim_time_us += 5000000ULL;
    sched_load_tick(rq, g_sim_time_us);
    sched_get_loadavg(loads, &nr_run, &nr_tot);
    assert(loads[0] > 0U);
    assert(nr_run == 1U);
    assert(nr_tot >= 1U);
}

static void test_pi_boost_and_multi_lock_unboost(void)
{
    struct task_struct t_owner, t_mid, t_waiter1, t_waiter2;
    struct mutex m1, m2;

    reset_test_env();
    memset(&m1, 0, sizeof(m1));
    memset(&m2, 0, sizeof(m2));
    INIT_LIST_HEAD(&m1.wait_list);
    INIT_LIST_HEAD(&m1.held_node);
    INIT_LIST_HEAD(&m2.wait_list);
    INIT_LIST_HEAD(&m2.held_node);

    init_mock_task(&t_owner, "owner", 60, SCHED_POLICY_RR, 20, 0, 5);
    init_mock_task(&t_mid, "mid", 61, SCHED_POLICY_RR, 12, 0, 5);
    init_mock_task(&t_waiter1, "w1", 62, SCHED_POLICY_RR, 10, 0, 5);
    init_mock_task(&t_waiter2, "w2", 63, SCHED_POLICY_RR, 4, 0, 5);

    add_task_to_ready_list(&t_owner);
    add_task_to_ready_list(&t_mid);
    switch_task();
    assert(task_current() == &t_mid);

    /* Owner holds m1 (waiter1 prio 10) and m2 (waiter2 prio 4) */
    list_add_tail(&m1.held_node, &t_owner.held_mutex_list);
    list_add_tail(&t_waiter1.list, &m1.wait_list);
    task_pi_boost(&t_owner, t_waiter1.current_priority);
    assert(t_owner.current_priority == 10);
    switch_task();
    assert(task_current() == &t_owner);

    list_add_tail(&m2.held_node, &t_owner.held_mutex_list);
    list_add_tail(&t_waiter2.list, &m2.wait_list);
    task_pi_boost(&t_owner, t_waiter2.current_priority);
    assert(t_owner.current_priority == 4);

    /* Changing base priority via task_set_prio while boosted updates init_priority */
    task_set_prio(&t_owner, 18);
    assert(t_owner.init_priority == 18);
    assert(t_owner.current_priority == 4);

    /* Releasing m2 drops owner to m1's waiter1 priority (10), still above t_mid (12) */
    list_del_init(&m2.held_node);
    task_pi_unboost(&t_owner, TASK_PRIO_INVALID);
    assert(t_owner.current_priority == 10);
    switch_task();
    assert(task_current() == &t_owner);

    /* Releasing m1 restores owner to updated init_priority (18), letting t_mid (12) run */
    list_del_init(&m1.held_node);
    task_pi_unboost(&t_owner, TASK_PRIO_INVALID);
    assert(t_owner.current_priority == 18);
    switch_task();
    assert(task_current() == &t_mid);
}

int main(void)
{
    test_rt_preemption_fifo_and_rr();
    test_fair_vruntime_weights_and_rt_preempt();
    test_deferred_resched_under_sch_lock();
    test_smp_select_rq_ipi_and_work_stealing();
    test_pelt_and_loadavg();
    test_pi_boost_and_multi_lock_unboost();
    puts("multi-policy scheduler and SMP runqueue checks passed");
    return 0;
}
