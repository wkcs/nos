/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 Stage 2A 自检套件：
 * 1. 验证 4 核 PSCI SMP 上线与跨核亲和性调度；
 * 2. 验证真实硬件 EL0t 用户态降级、TTBR0 + 16-bit ASID 独立映射与 SVC #0 系统调用；
 * 3. 验证只读页硬件权限违例（EL0 Data Abort）仅隔离终止该任务而内核不受影响。
 */

#include <kernel/kernel.h>
#include <kernel/init.h>
#include <kernel/mm.h>
#include <kernel/task.h>
#include <kernel/sched.h>
#include <kernel/printk.h>
#include <nos/hal_arch.h>
#include <nos/completion.h>
#include <asm/mmu.h>
#include <asm/switch.h>
#include <asm/sysreg.h>
#include <string.h>

#define SELFTEST_USER_CODE_VA   0x0000000000400000UL
#define SELFTEST_USER_RO_VA     0x0000000000410000UL
#define SELFTEST_USER_STACK_VA  0x0000000000800000UL
#define SELFTEST_RO_MAGIC       0x4E4F534149303031ULL

extern uint32_t arm64_smp_online_count(void);
extern const uint8_t arm64_el0_smoke_code_start[];
extern const uint8_t arm64_el0_smoke_code_end[];
extern const uint8_t arm64_el0_fault_code_start[];
extern const uint8_t arm64_el0_fault_code_end[];

struct smp_worker_ctx {
    uint32_t               target_cpu;
    uint32_t              *observed_mask;
    struct nos_completion  done;
};

struct el0_test_ctx {
    addr_t                code_va;
    addr_t                ro_va;
    addr_t                stack_va;
    struct nos_completion cleaned_done;
};

static uint32_t g_el0_fault_hits;
static uint32_t g_el0_fault_ec;
static uint64_t g_el0_fault_far;

static void selftest_fault_observer(uint32_t ec, uint64_t far, uint64_t pc)
{
    (void)pc;
    __atomic_store_n(&g_el0_fault_ec, ec, __ATOMIC_RELAXED);
    __atomic_store_n(&g_el0_fault_far, far, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_el0_fault_hits, 1U, __ATOMIC_RELEASE);
}

static void smp_worker_entry(void *param)
{
    struct smp_worker_ctx *ctx = (struct smp_worker_ctx *)param;
    uint32_t actual = hal_cpu_id();

    if (actual == ctx->target_cpu) {
        __atomic_fetch_or(ctx->observed_mask, (1U << actual), __ATOMIC_ACQ_REL);
    } else {
        pr_err("Stage2A SMP mismatch: expected CPU%u, ran on CPU%u\n",
               (unsigned int)ctx->target_cpu, (unsigned int)actual);
    }
    (void)nos_complete(&ctx->done);
}

static int run_smp_verification(uint32_t expected_cpus)
{
    static struct smp_worker_ctx workers[8];
    uint32_t observed_mask = 0U;
    uint32_t expected_mask = 0U;
    uint32_t count = expected_cpus;

    if (count > 8U) {
        count = 8U;
    }

    for (uint32_t cpu = 0U; cpu < count; cpu++) {
        struct task_struct *t;
        expected_mask |= (1U << cpu);
        workers[cpu].target_cpu = cpu;
        workers[cpu].observed_mask = &observed_mask;
        (void)nos_completion_init(&workers[cpu].done);

        t = task_create_ex("smp_probe",
                           smp_worker_entry,
                           &workers[cpu],
                           SCHED_POLICY_RR,
                           4U,
                           0,
                           (uint8_t)(1U << cpu),
                           CONFIG_CORE_TASK_STACK_SIZE,
                           10U,
                           NULL);
        if (t == NULL) {
            return -ENOMEM;
        }
        (void)task_ready(t);
    }

    for (uint32_t cpu = 0U; cpu < count; cpu++) {
        int rc = nos_completion_wait(&workers[cpu].done, 3000U);
        (void)nos_object_close(&workers[cpu].done.object);
        (void)nos_object_retire(&workers[cpu].done.object);
        if (rc != 0) {
            pr_err("Stage2A SMP timeout on CPU%u (rc=%d)\n", (unsigned int)cpu, rc);
            return -ETIMEDOUT;
        }
    }

    if (__atomic_load_n(&observed_mask, __ATOMIC_ACQUIRE) != expected_mask) {
        pr_err("Stage2A SMP mask mismatch: got=0x%x expected=0x%x\n",
               (unsigned int)observed_mask, (unsigned int)expected_mask);
        return -EIO;
    }
    return 0;
}

static void el0_task_cleanup(struct task_struct *task)
{
    struct el0_test_ctx *ctx;

    if (task == NULL || task->parameter == NULL) {
        return;
    }
    ctx = (struct el0_test_ctx *)task->parameter;
    if (ctx->code_va != 0U) {
        free_page(ctx->code_va);
        ctx->code_va = 0U;
    }
    if (ctx->ro_va != 0U) {
        free_page(ctx->ro_va);
        ctx->ro_va = 0U;
    }
    if (ctx->stack_va != 0U) {
        free_page(ctx->stack_va);
        ctx->stack_va = 0U;
    }
    (void)nos_complete(&ctx->cleaned_done);
}

static void el0_launcher_entry(void *param)
{
    (void)param;
    arm64_enter_user_mode(SELFTEST_USER_CODE_VA,
                          SELFTEST_USER_STACK_VA + PAGE_SIZE,
                          SELFTEST_USER_RO_VA);
}

static int run_el0_scenario(const char *name,
                            const uint8_t *code_begin,
                            const uint8_t *code_end)
{
    static struct el0_test_ctx ctx;
    struct task_struct *t;
    size_t code_len = (size_t)(code_end - code_begin);
    static const char msg[] = "[EL0] Hello via SVC #0!\n";
    uint64_t *ro_hdr;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    (void)nos_completion_init(&ctx.cleaned_done);

    ctx.code_va = alloc_page(GFP_KERNEL | GFP_ZERO);
    ctx.ro_va = alloc_page(GFP_KERNEL | GFP_ZERO);
    ctx.stack_va = alloc_page(GFP_KERNEL | GFP_ZERO);
    if (ctx.code_va == 0U || ctx.ro_va == 0U || ctx.stack_va == 0U) {
        if (ctx.code_va != 0U) free_page(ctx.code_va);
        if (ctx.ro_va != 0U) free_page(ctx.ro_va);
        if (ctx.stack_va != 0U) free_page(ctx.stack_va);
        return -ENOMEM;
    }

    if (code_len > PAGE_SIZE) {
        free_page(ctx.code_va);
        free_page(ctx.ro_va);
        free_page(ctx.stack_va);
        return -E2BIG;
    }

    memcpy((void *)ctx.code_va, code_begin, code_len);
    arm64_sync_icache_dcache((uintptr_t)ctx.code_va, PAGE_SIZE);

    ro_hdr = (uint64_t *)ctx.ro_va;
    ro_hdr[0] = SELFTEST_RO_MAGIC;
    ro_hdr[1] = (uint64_t)(sizeof(msg) - 1U);
    memcpy((void *)&ro_hdr[2], msg, sizeof(msg));
    arm64_clean_dcache_poc((uintptr_t)ctx.ro_va, PAGE_SIZE);

    t = task_create_ex(name,
                       el0_launcher_entry,
                       &ctx,
                       SCHED_POLICY_RR,
                       5U,
                       0,
                       0x01U,
                       CONFIG_CORE_TASK_STACK_SIZE,
                       10U,
                       el0_task_cleanup);
    if (t == NULL) {
        free_page(ctx.code_va);
        free_page(ctx.ro_va);
        free_page(ctx.stack_va);
        return -ENOMEM;
    }

    rc = hal_vm_map(&t->vm_space,
                    SELFTEST_USER_CODE_VA,
                    arm64_virt_to_phys((uintptr_t)ctx.code_va),
                    PAGE_SIZE,
                    HAL_VM_PROT_USER | HAL_VM_PROT_READ | HAL_VM_PROT_EXEC);
    if (rc == 0) {
        rc = hal_vm_map(&t->vm_space,
                        SELFTEST_USER_RO_VA,
                        arm64_virt_to_phys((uintptr_t)ctx.ro_va),
                        PAGE_SIZE,
                        HAL_VM_PROT_USER | HAL_VM_PROT_READ);
    }
    if (rc == 0) {
        rc = hal_vm_map(&t->vm_space,
                        SELFTEST_USER_STACK_VA,
                        arm64_virt_to_phys((uintptr_t)ctx.stack_va),
                        PAGE_SIZE,
                        HAL_VM_PROT_USER | HAL_VM_PROT_READ | HAL_VM_PROT_WRITE);
    }
    if (rc != 0) {
        task_del(t);
        (void)nos_completion_wait(&ctx.cleaned_done, 3000U);
        (void)nos_object_close(&ctx.cleaned_done.object);
        (void)nos_object_retire(&ctx.cleaned_done.object);
        return rc;
    }

    (void)task_ready(t);
    rc = nos_completion_wait(&ctx.cleaned_done, 5000U);
    (void)nos_object_close(&ctx.cleaned_done.object);
    (void)nos_object_retire(&ctx.cleaned_done.object);
    return rc;
}

static int stage2_selftest_init(void)
{
    uint32_t online = arm64_smp_online_count();
    int rc;

    rc = run_smp_verification(online);
    if (rc != 0) {
        pr_err("ARM64 Stage2A SMP selftest FAILED (%d)\n", rc);
        return rc;
    }

    rc = run_el0_scenario("el0_svc",
                          arm64_el0_smoke_code_start,
                          arm64_el0_smoke_code_end);
    if (rc != 0) {
        pr_err("ARM64 Stage2A EL0 SVC selftest FAILED (%d)\n", rc);
        return rc;
    }

    __atomic_store_n(&g_el0_fault_hits, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&g_el0_fault_ec, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&g_el0_fault_far, 0ULL, __ATOMIC_RELAXED);
    arm64_set_el0_fault_observer(selftest_fault_observer);

    rc = run_el0_scenario("el0_fault",
                          arm64_el0_fault_code_start,
                          arm64_el0_fault_code_end);
    arm64_set_el0_fault_observer(NULL);

    if (rc != 0 ||
        __atomic_load_n(&g_el0_fault_hits, __ATOMIC_ACQUIRE) != 1U ||
        g_el0_fault_ec != ESR_ELx_EC_DABT_LOW ||
        g_el0_fault_far != (uint64_t)SELFTEST_USER_RO_VA) {
        pr_err("ARM64 Stage2A EL0 Fault selftest FAILED (rc=%d hits=%u ec=0x%x far=0x%lx)\n",
               rc, (unsigned int)g_el0_fault_hits,
               (unsigned int)g_el0_fault_ec, (unsigned long)g_el0_fault_far);
        return -EFAULT;
    }

    pr_info("ARM64 Stage2A selftest PASS (smp=%u/%u, el0_svc=ok, el0_fault_isolated=ok)\n",
            (unsigned int)online, (unsigned int)online);
    return 0;
}
task_init(stage2_selftest_init);
