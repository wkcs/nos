/**
 * Copyright (C) 2026 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <kernel/kernel.h>
#include <kernel/kallsyms.h>
#include <kernel/task.h>
#include <kernel/sched.h>
#include <kernel/unwind.h>
#include <nos/sched.h>

/* Mock BPE token table: 0x80 -> "wo", 0x81 -> (0x80, 'r') -> "wor" */
const unsigned char kallsyms_token_table[256] = {
    'w', 'o', 0x80U, 'r', 0U
};

/* Mock sorted kernel symbol table */
const struct kallsym_entry kallsyms_table[] = {
    { 0x08001000UL, "sched_init" },
    { 0x08001080UL, "switch_task" },
    { 0x08001180UL, "mutex_lock" },
    { 0x08001240UL, "sem_get" },
    { 0x08001300UL, "task_sleep" },
    { 0x08001400UL, "\x81" "ker_step" },
    { 0x08001500UL, "\x81" "ker_main" },
    { 0UL, NULL }
};
const unsigned int kallsyms_num = 7U;

char _etext[4];
addr_t kernel_running = 1U;
uint32_t scheduler_lock_nest = 0U;
u32 g_sys_cycle = 1U;
u64 sys_heartbeat_time = 0ULL;

static struct task_struct g_test_tasks[2];
static size_t g_test_task_count = 0U;
static struct task_struct *g_current_task = NULL;
static uint64_t g_mock_now_us = 0ULL;

static char g_print_buf[8192];
static size_t g_print_pos = 0U;

static void reset_print_buf(void)
{
    g_print_pos = 0U;
    g_print_buf[0] = '\0';
}

static int capture_print(const char *fmt, ...)
{
    va_list args;
    int n;

    va_start(args, fmt);
    if (g_print_pos < sizeof(g_print_buf)) {
        n = vsnprintf(g_print_buf + g_print_pos,
                      sizeof(g_print_buf) - g_print_pos,
                      fmt,
                      args);
        if (n > 0) {
            g_print_pos += (size_t)n;
            if (g_print_pos >= sizeof(g_print_buf)) {
                g_print_pos = sizeof(g_print_buf) - 1U;
            }
        }
    } else {
        n = 0;
    }
    va_end(args);
    return n;
}

int vprintk(const char *fmt, va_list args)
{
    return vsnprintf(g_print_buf + g_print_pos,
                     (g_print_pos < sizeof(g_print_buf)) ? (sizeof(g_print_buf) - g_print_pos) : 0U,
                     fmt,
                     args);
}

int pr_log(bool no_tag, enum log_level level, const char *fmt, ...)
{
    va_list args;
    int n;
    (void)no_tag;
    (void)level;
    va_start(args, fmt);
    n = vprintk(fmt, args);
    va_end(args);
    return n;
}

void panic(const char *fmt, ...)
{
    (void)fmt;
    assert(0 && "unexpected panic");
}

addr_t disable_irq_save(void)
{
    return 0U;
}

void enable_irq_save(addr_t level)
{
    (void)level;
}

struct task_struct *task_current(void)
{
    return g_current_task;
}

struct task_struct *task_find_by_pid(pid_t pid)
{
    size_t i;
    for (i = 0U; i < g_test_task_count; i++) {
        if (g_test_tasks[i].pid == pid) {
            return &g_test_tasks[i];
        }
    }
    return NULL;
}

int task_foreach(int (*cb)(struct task_struct *task, void *arg), void *arg)
{
    size_t i;
    int rc = 0;
    for (i = 0U; i < g_test_task_count; i++) {
        rc = cb(&g_test_tasks[i], arg);
        if (rc != 0) {
            break;
        }
    }
    return rc;
}

size_t task_stack_max_used(struct task_struct *task)
{
    uintptr_t low = 0U;
    uintptr_t high = 0U;
    size_t i;

    if (task == NULL || task->stack == NULL || task->stack_size == 0U) {
        return 0U;
    }
    task_get_stack_bounds(task, &low, &high);
    if (low == 0U || high <= low || *(const char *)low != '#') {
        return 0U;
    }
    for (i = 0U; i < task->stack_size; i++) {
        if (((const char *)low)[i] != '#') {
            return task->stack_size - i;
        }
    }
    return 0U;
}

u64 cpu_run_time_us(void)
{
    return g_mock_now_us;
}

/* Weak scheduler/PELT stubs in case kernel/sched/sched_load.c is not linked */
__attribute__((weak)) struct sched_rq g_sched_rqs[CONFIG_NR_CPUS];
static uint32_t g_fallback_avenrun[3];
static uint64_t g_fallback_last_load_us;

__attribute__((weak)) struct sched_rq *sched_rq_of(uint32_t cpu_id)
{
    if (cpu_id >= (uint32_t)CONFIG_NR_CPUS) {
        return NULL;
    }
    return &g_sched_rqs[cpu_id];
}

__attribute__((weak)) struct sched_rq *this_rq(void)
{
    return &g_sched_rqs[0];
}

__attribute__((weak)) uint16_t sched_nice_to_weight(int8_t nice)
{
    (void)nice;
    return 1024U;
}

__attribute__((weak)) void sched_load_init(void)
{
    memset(g_fallback_avenrun, 0, sizeof(g_fallback_avenrun));
    g_fallback_last_load_us = 0ULL;
}

__attribute__((weak)) void sched_load_task_init(struct task_struct *p, int8_t nice)
{
    if (p == NULL) {
        return;
    }
    p->se.vruntime = 0ULL;
    p->se.sum_exec_us = 0ULL;
    p->se.exec_start_us = 0ULL;
    p->se.win_start_us = 0U;
    p->se.win_run_us = 0U;
    p->se.util_avg = 0U;
    p->se.weight = sched_nice_to_weight(nice);
}

__attribute__((weak)) void sched_load_decay_task(struct task_struct *p, uint64_t now_us)
{
    uint32_t now32 = (uint32_t)now_us;
    uint32_t elapsed;
    uint32_t windows;
    uint32_t i;

    if (p == NULL) {
        return;
    }
    elapsed = now32 - p->se.win_start_us;
    if (elapsed < SCHED_PELT_WINDOW_US) {
        return;
    }
    windows = elapsed / SCHED_PELT_WINDOW_US;
    if (windows > 8U) {
        windows = 8U;
    }
    for (i = 0U; i < windows; i++) {
        uint32_t win_util = 0U;
        if (i == 0U && p->se.win_run_us > 0U) {
            win_util = (p->se.win_run_us * SCHED_CAPACITY_SCALE) / SCHED_PELT_WINDOW_US;
            if (win_util > SCHED_CAPACITY_SCALE) {
                win_util = SCHED_CAPACITY_SCALE;
            }
        }
        p->se.util_avg = (uint16_t)(((uint32_t)p->se.util_avg * 3U + win_util) >> 2);
    }
    p->se.win_start_us = now32 - (elapsed % SCHED_PELT_WINDOW_US);
    p->se.win_run_us = 0U;
}

__attribute__((weak)) void sched_load_update_task(struct sched_rq *rq,
                                                  struct task_struct *p,
                                                  uint64_t now_us,
                                                  uint64_t delta_us,
                                                  bool is_idle)
{
    uint32_t now32 = (uint32_t)now_us;

    if (p != NULL && !is_idle) {
        uint32_t elapsed;
        p->se.sum_exec_us += delta_us;
        p->se.win_run_us += (uint32_t)delta_us;
        elapsed = now32 - p->se.win_start_us;
        if (elapsed >= SCHED_PELT_WINDOW_US) {
            uint32_t win_util = ((uint64_t)p->se.win_run_us * SCHED_CAPACITY_SCALE) / elapsed;
            if (win_util > SCHED_CAPACITY_SCALE) {
                win_util = SCHED_CAPACITY_SCALE;
            }
            p->se.util_avg = (uint16_t)(((uint32_t)p->se.util_avg * 3U + win_util) >> 2);
            p->se.win_start_us = now32;
            p->se.win_run_us = 0U;
        }
    }
    if (rq != NULL) {
        uint32_t rq_elapsed;
        rq->clock_us = now_us;
        if (is_idle) {
            rq->idle_time_us += delta_us;
        } else {
            rq->busy_time_us += delta_us;
            rq->win_busy_us += (uint32_t)delta_us;
        }
        rq_elapsed = now32 - rq->win_start_us;
        if (rq_elapsed >= SCHED_PELT_WINDOW_US) {
            uint32_t rq_util = ((uint64_t)rq->win_busy_us * SCHED_CAPACITY_SCALE) / rq_elapsed;
            if (rq_util > SCHED_CAPACITY_SCALE) {
                rq_util = SCHED_CAPACITY_SCALE;
            }
            rq->cpu_util_avg = (uint16_t)(((uint32_t)rq->cpu_util_avg * 3U + rq_util) >> 2);
            rq->win_start_us = now32;
            rq->win_busy_us = 0U;
        }
    }
}

__attribute__((weak)) void sched_load_tick(struct sched_rq *rq, uint64_t now_us)
{
    static const uint32_t exp_factors[3] = { 1884U, 2014U, 2037U };
    uint32_t active;
    int i;

    if (rq == NULL) {
        return;
    }
    if (now_us - g_fallback_last_load_us < 5000000ULL) {
        return;
    }
    g_fallback_last_load_us = now_us;
    active = (rq->nr_running + rq->nr_uninterruptible) * SCHED_FIXED_1;
    for (i = 0; i < 3; i++) {
        g_fallback_avenrun[i] = (g_fallback_avenrun[i] * exp_factors[i] +
                                 active * (SCHED_FIXED_1 - exp_factors[i])) >> SCHED_FIXED_SHIFT;
    }
}

__attribute__((weak)) uint16_t sched_load_task_util(struct task_struct *p, uint64_t now_us)
{
    (void)now_us;
    return p ? p->se.util_avg : 0U;
}

__attribute__((weak)) uint16_t sched_load_rq_util(struct sched_rq *rq, uint64_t now_us)
{
    (void)now_us;
    return rq ? rq->cpu_util_avg : 0U;
}

__attribute__((weak)) void sched_get_loadavg(uint32_t loads[3],
                                             uint32_t *nr_running,
                                             uint32_t *nr_total)
{
    if (loads != NULL) {
        loads[0] = g_fallback_avenrun[0];
        loads[1] = g_fallback_avenrun[1];
        loads[2] = g_fallback_avenrun[2];
    }
    if (nr_running != NULL) {
        *nr_running = g_sched_rqs[0].nr_running;
    }
    if (nr_total != NULL) {
        *nr_total = (uint32_t)g_test_task_count;
    }
}

__attribute__((weak)) u32 task_get_cpu_usage(struct task_struct *task)
{
    uint16_t util;
    if (task == NULL) {
        return 0U;
    }
    util = sched_load_task_util(task, g_mock_now_us);
    return ((u32)util * 10000U) >> SCHED_CAPACITY_SHIFT;
}

__attribute__((weak)) u32 get_cpu_usage(void)
{
    uint16_t util = sched_load_rq_util(&g_sched_rqs[0], g_mock_now_us);
    return ((u32)util * 10000U) >> SCHED_CAPACITY_SHIFT;
}

static void test_kallsyms_binary_search(void)
{
    unsigned long size = 0UL;
    unsigned long offset = 0UL;
    const char *name;
    char buf[64];

    /* Exact match on first entry */
    name = kallsyms_lookup(0x08001000UL, &size, &offset, NULL, NULL);
    assert(name != NULL && strcmp(name, "sched_init") == 0);
    assert(offset == 0UL && size == 0x80UL);

    /* Interior match with ARM Thumb bit 0 set */
    name = kallsyms_lookup(0x08001095UL, &size, &offset, NULL, NULL);
    assert(name != NULL && strcmp(name, "switch_task") == 0);
    assert(offset == 0x14UL && size == 0x100UL);

    /* Match on last entry (dynamic size == 0) */
    name = kallsyms_lookup(0x08001508UL, &size, &offset, NULL, NULL);
    assert(name != NULL && strcmp(name, "worker_main") == 0);
    assert(offset == 0x8UL && size == 0UL);

    /* Out-of-range below first symbol or far beyond last symbol */
    assert(kallsyms_lookup(0x08000F00UL, &size, &offset, NULL, NULL) == NULL);
    assert(kallsyms_lookup(0x08008000UL, &size, &offset, NULL, NULL) == NULL);

    /* Sprint formatting */
    kallsyms_sprint_symbol(buf, sizeof(buf), 0x08001095UL);
    assert(strcmp(buf, "switch_task+0x14/0x100") == 0);

    kallsyms_sprint_symbol(buf, sizeof(buf), 0x08001508UL);
    assert(strcmp(buf, "worker_main+0x8") == 0);

    kallsyms_sprint_symbol(buf, sizeof(buf), 0x20000100UL);
    assert(strcmp(buf, "0x20000100") == 0);
}

static void test_unwind_fp_and_thumb_scanner(void)
{
    uintptr_t fake_stack[64];
    uint16_t fake_text[128];
    struct unwind_frame frame;
    uintptr_t pcs[8];
    size_t n;
    uintptr_t text_start;
    uintptr_t text_end;
    uintptr_t ret_bl32;
    uintptr_t ret_blx16;
    uintptr_t bogus_ret;

    /* 1. Frame Pointer chain unwinding */
    memset(fake_stack, 0, sizeof(fake_stack));
    fake_stack[10] = (uintptr_t)&fake_stack[20]; /* next_fp */
    fake_stack[11] = 0x08001421UL;               /* ret_lr (Thumb) */
    fake_stack[20] = (uintptr_t)&fake_stack[30]; /* next_fp */
    fake_stack[21] = 0x08001511UL;               /* ret_lr (Thumb) */
    fake_stack[30] = 0U;                         /* end of chain */
    fake_stack[31] = 0x08001011UL;

    memset(&frame, 0, sizeof(frame));
    frame.pc = 0x08001190UL;
    frame.sp = (uintptr_t)&fake_stack[8];
    frame.fp = (uintptr_t)&fake_stack[10];
    frame.stack_low = (uintptr_t)&fake_stack[0];
    frame.stack_high = (uintptr_t)&fake_stack[64];

    n = unwind_frames_from_state(&frame, 0x08001000UL, 0x08002000UL, false, pcs, 8U);
    assert(n == 4U);
    assert(pcs[0] == 0x08001190UL);
    assert(pcs[1] == 0x08001420UL);
    assert(pcs[2] == 0x08001510UL);
    assert(pcs[3] == 0x08001010UL);

    /* 2. Thumb-2 BL/BLX instruction-verified stack scanner */
    memset(fake_text, 0, sizeof(fake_text));
    text_start = (uintptr_t)&fake_text[0];
    text_end = (uintptr_t)&fake_text[128];

    /* Valid 32-bit Thumb-2 BL at index 16..17, return address at index 18 */
    fake_text[16] = 0xF000U;
    fake_text[17] = 0xD800U;
    ret_bl32 = ((uintptr_t)&fake_text[18]) | 1U;

    /* Valid 16-bit Thumb BLX r3 at index 40, return address at index 41 */
    fake_text[40] = 0x4798U;
    ret_blx16 = ((uintptr_t)&fake_text[41]) | 1U;

    /* Non-call instruction at index 60..61, bogus code pointer at index 62 */
    fake_text[60] = 0x2000U;
    fake_text[61] = 0x2001U;
    bogus_ret = ((uintptr_t)&fake_text[62]) | 1U;

    memset(fake_stack, 0, sizeof(fake_stack));
    fake_stack[4] = 0xDEADBEEFUL;                 /* non-code noise */
    fake_stack[5] = bogus_ret;                    /* code address NOT preceded by BL/BLX */
    fake_stack[6] = (uintptr_t)&fake_text[18];    /* missing Thumb bit 0 */
    fake_stack[7] = ret_bl32;                     /* valid 32-bit BL return address */
    fake_stack[8] = ret_bl32;                     /* duplicate adjacent return address */
    fake_stack[9] = ret_blx16;                    /* valid 16-bit BLX return address */

    memset(&frame, 0, sizeof(frame));
    frame.pc = ((uintptr_t)&fake_text[4]) | 1U;
    frame.sp = (uintptr_t)&fake_stack[4];
    frame.fp = 0U;
    frame.stack_low = (uintptr_t)&fake_stack[0];
    frame.stack_high = (uintptr_t)&fake_stack[32];

    n = unwind_frames_from_state(&frame, text_start, text_end, true, pcs, 8U);
    assert(n == 3U);
    assert(pcs[0] == ((uintptr_t)&fake_text[4] & ~(uintptr_t)1U));
    assert(pcs[1] == (ret_bl32 & ~(uintptr_t)1U));
    assert(pcs[2] == (ret_blx16 & ~(uintptr_t)1U));
}

static void test_pelt_and_ps_dump(void)
{
    static addr_t stack0[256];
    static addr_t stack1[256];
    uint32_t loads[3] = { 0U, 0U, 0U };
    uint32_t nr_run = 0U;
    uint32_t nr_tot = 0U;
    int i;

    memset(stack0, '#', sizeof(stack0));
    memset(stack1, '#', sizeof(stack1));
    /* Simulate 128 bytes of stack usage at top of downward-growing stack */
    memset((char *)stack0 + sizeof(stack0) - 128U, 0xA5, 128U);
    memset((char *)stack1 + sizeof(stack1) - 192U, 0x5A, 192U);

    memset(g_test_tasks, 0, sizeof(g_test_tasks));
    memset(&g_sched_rqs[0], 0, sizeof(g_sched_rqs[0]));
    sched_load_init();

    g_test_task_count = 2U;
    g_test_tasks[0].pid = 1;
    g_test_tasks[0].ppid = 0;
    g_test_tasks[0].name = "worker_rt";
    g_test_tasks[0].policy = SCHED_POLICY_RR;
    g_test_tasks[0].status = TASK_RUNNING;
    g_test_tasks[0].init_priority = 4;
    g_test_tasks[0].current_priority = 4;
    g_test_tasks[0].cpus_allowed = 0x01U;
    g_test_tasks[0].stack_size = sizeof(stack0);
    g_test_tasks[0].stack = &stack0[255];
    g_test_tasks[0].sp = &stack0[240];
    sched_load_task_init(&g_test_tasks[0], 0);

    g_test_tasks[1].pid = 2;
    g_test_tasks[1].ppid = 1;
    g_test_tasks[1].name = "sleeper_cfs";
    g_test_tasks[1].policy = SCHED_POLICY_NORMAL;
    g_test_tasks[1].status = TASK_BLOCKED;
    g_test_tasks[1].init_priority = 10;
    g_test_tasks[1].current_priority = 10;
    g_test_tasks[1].nice = -2;
    g_test_tasks[1].cpus_allowed = 0x03U;
    g_test_tasks[1].stack_size = sizeof(stack1);
    g_test_tasks[1].stack = &stack1[255];
    g_test_tasks[1].sp = &stack1[220];
    g_test_tasks[1].wchan_name = "mutex";
    g_test_tasks[1].wchan_addr = 0x08001190UL;
    g_test_tasks[1].se.nvcsw = 12U;
    g_test_tasks[1].se.nivcsw = 3U;
    g_test_tasks[1].se.vruntime = 45678ULL;
    sched_load_task_init(&g_test_tasks[1], -2);
    g_test_tasks[1].se.nvcsw = 12U;
    g_test_tasks[1].se.nivcsw = 3U;
    g_test_tasks[1].se.vruntime = 45678ULL;

    g_current_task = &g_test_tasks[0];
    g_sched_rqs[0].cpu_id = 0U;
    g_sched_rqs[0].nr_running = 2U;
    g_sched_rqs[0].nr_uninterruptible = 1U;
    g_sched_rqs[0].curr = &g_test_tasks[0];

    /* Accumulate 8 full PELT windows (800 ms) of 100% active execution */
    g_mock_now_us = 0ULL;
    for (i = 0; i < 8; i++) {
        g_mock_now_us += SCHED_PELT_WINDOW_US;
        sched_load_update_task(&g_sched_rqs[0],
                               &g_test_tasks[0],
                               g_mock_now_us,
                               SCHED_PELT_WINDOW_US,
                               false);
    }
    assert(g_test_tasks[0].se.util_avg > 800U);
    assert(task_get_cpu_usage(&g_test_tasks[0]) > 7500U);
    assert(get_cpu_usage() > 7500U);

    /* Decay across 4 idle windows */
    uint16_t peak_util = g_test_tasks[0].se.util_avg;
    g_mock_now_us += 4ULL * SCHED_PELT_WINDOW_US;
    sched_load_decay_task(&g_test_tasks[0], g_mock_now_us);
    assert(g_test_tasks[0].se.util_avg < peak_util);
    assert(g_test_tasks[0].se.util_avg > 0U);

    /* Trigger 5-second loadavg sample */
    g_mock_now_us = 5000000ULL;
    sched_load_tick(&g_sched_rqs[0], g_mock_now_us);
    sched_get_loadavg(loads, &nr_run, &nr_tot);
    assert(loads[0] > 0U);
    assert(nr_run == 2U);

    /* Verify TASK_PS_NORMAL output */
    reset_print_buf();
    dump_all_task_ex(capture_print, TASK_PS_NORMAL);
    assert(strstr(g_print_buf, "Tasks: 2 total, 1 run, 1 sleep") != NULL);
    assert(strstr(g_print_buf, "PID PPID CPU POL PRIO STAT") != NULL);
    assert(strstr(g_print_buf, "worker_rt") != NULL);
    assert(strstr(g_print_buf, "sleeper_cfs") != NULL);
    assert(strstr(g_print_buf, "mutex") != NULL);

    /* Verify TASK_PS_LONG output */
    reset_print_buf();
    dump_all_task_ex(capture_print, TASK_PS_LONG);
    assert(strstr(g_print_buf, "VRUNTIME") != NULL);
    assert(strstr(g_print_buf, "45678") != NULL);
    assert(strstr(g_print_buf, "0x03") != NULL);

    /* Verify TASK_PS_STACK and dump_task_detail */
    reset_print_buf();
    dump_all_task_ex(capture_print, TASK_PS_STACK);
    assert(strstr(g_print_buf, "Call Trace (task=sleeper_cfs") != NULL);
    assert(strstr(g_print_buf, "mutex_lock+0x10/0xc0") != NULL);

    reset_print_buf();
    assert(dump_task_detail(2, capture_print, true) == 0);
    assert(strstr(g_print_buf, "Task: sleeper_cfs (PID=2, PPID=1, State=D, Policy=TS)") != NULL);
    assert(dump_task_detail(99, capture_print, false) == -ESRCH);

    reset_print_buf();
    dump_sched_rq(capture_print);
    assert(strstr(g_print_buf, "CPU0: nr_run=2") != NULL);
}

int main(void)
{
    test_kallsyms_binary_search();
    test_unwind_fp_and_thumb_scanner();
    test_pelt_and_ps_dump();
    puts("sched load, kallsyms binary search, dual-mode unwinder, and ps dump checks passed");
    return 0;
}
