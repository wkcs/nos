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
#ifdef CONFIG_NOS_AI_CORE
#include <nos/cap.h>
#include <nos/vmo.h>
#include <nos/cell.h>
#include <uapi/nos/ai.h>
#endif
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
#ifdef CONFIG_NOS_AI_CORE
extern const uint8_t arm64_el0_cell_code_start[];
extern const uint8_t arm64_el0_cell_code_end[];
extern const uint8_t arm64_el0_cell_tamper_code_start[];
extern const uint8_t arm64_el0_cell_tamper_code_end[];

#define SELFTEST_CELL_CODE_VA   0x0000000000400000UL
#define SELFTEST_CELL_TELEM_VA  0x0000000000420000UL
#define SELFTEST_CELL_VMO_VA    0x0000000000430000UL
#define SELFTEST_CELL_STACK_VA  0x0000000000800000UL
#define SELFTEST_CELL_VMO_MAGIC 0x4E4F534149303032ULL
#endif

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

#ifdef CONFIG_NOS_AI_CORE
static void el0_cell_launcher_entry(void *param)
{
    (void)param;
    arm64_enter_user_mode(SELFTEST_CELL_CODE_VA,
                          SELFTEST_CELL_STACK_VA + PAGE_SIZE,
                          SELFTEST_CELL_TELEM_VA);
}

static void el0_cell_task_cleanup(struct task_struct *task)
{
    if (task != NULL && task->parameter != NULL) {
        (void)nos_complete((struct nos_completion *)task->parameter);
    }
}

static int run_stage2b_selftest(void)
{
    static const char cell_msg[] = "[EL0-CELL] Telemetry & Sealed VMO OK!\n";
    static struct nos_completion run_done;
    static struct nos_completion tamper_done;
    struct nos_cell_budget sup_b = {
        .cpu_ns_limit = 2000000000ULL,
        .mem_bytes_limit = 262144ULL,
        .tool_calls_limit = 64ULL,
        .accel_ops_limit = 64ULL,
        .max_child_cells = 4U,
        .flags = 0U,
    };
    struct nos_cell_budget child_b = {
        .cpu_ns_limit = 500000000ULL,
        .mem_bytes_limit = 32768ULL,
        .tool_calls_limit = 8ULL,
        .accel_ops_limit = 8ULL,
        .max_child_cells = 0U,
        .flags = 0U,
    };
    struct nos_vmo_create_args wargs = {
        .size_bytes = PAGE_SIZE,
        .kind = NOS_VMO_WEIGHT,
        .flags = 0U,
        .block_bytes = 0U,
    };
    uint8_t weight_hdr[80];
    uint64_t *hdr64 = (uint64_t *)weight_hdr;
    struct nos_audit_record audit_recs[16];
    struct nos_cell *sup_cell = NULL;
    struct nos_cell *child_cell = NULL;
    struct nos_vmo *weight_vmo = NULL;
    nos_handle_t sup_vmo_h = NOS_HANDLE_INVALID;
    nos_handle_t child_vmo_h = NOS_HANDLE_INVALID;
    addr_t code_va = 0U;
    addr_t stack_va = 0U;
    struct task_struct *t1;
    struct task_struct *t2;
    size_t c1_len = (size_t)(arm64_el0_cell_code_end - arm64_el0_cell_code_start);
    size_t c2_len = (size_t)(arm64_el0_cell_tamper_code_end - arm64_el0_cell_tamper_code_start);
    bool seen_budget_hit = false;
    bool seen_fault = false;
    bool seen_cap_revoke = false;
    uint32_t revoked = 0U;
    int audit_n;
    int rc = 0;

    if (c1_len > PAGE_SIZE || c2_len > PAGE_SIZE) {
        return -E2BIG;
    }

    code_va = alloc_page(GFP_KERNEL | GFP_ZERO);
    stack_va = alloc_page(GFP_KERNEL | GFP_ZERO);
    if (code_va == 0U || stack_va == 0U) {
        rc = -ENOMEM;
        goto cleanup;
    }

    sup_cell = nos_cell_create(NULL, &sup_b);
    weight_vmo = nos_vmo_create(&wargs);
    if (sup_cell == NULL || weight_vmo == NULL) {
        rc = -ENOMEM;
        goto cleanup;
    }

    memset(weight_hdr, 0, sizeof(weight_hdr));
    hdr64[0] = SELFTEST_CELL_VMO_MAGIC;
    hdr64[1] = (uint64_t)(sizeof(cell_msg) - 1U);
    memcpy(&weight_hdr[16], cell_msg, sizeof(cell_msg));
    rc = nos_vmo_write(weight_vmo, 0U, weight_hdr, sizeof(weight_hdr));
    if (rc != 0) {
        goto cleanup;
    }
    rc = nos_vmo_seal(weight_vmo);
    if (rc != 0) {
        goto cleanup;
    }

    /* 验证硬件 W^X 隔离与 Sealed 权重不可变写保护 */
    if (nos_vmo_map(weight_vmo, nos_cell_vm_space(sup_cell), SELFTEST_CELL_VMO_VA, 0U,
                    PAGE_SIZE, HAL_VM_PROT_USER | HAL_VM_PROT_WRITE | HAL_VM_PROT_EXEC,
                    NULL) != -EPERM) {
        rc = -EPERM;
        goto cleanup;
    }
    if (nos_vmo_map(weight_vmo, nos_cell_vm_space(sup_cell), SELFTEST_CELL_VMO_VA, 0U,
                    PAGE_SIZE, HAL_VM_PROT_USER | HAL_VM_PROT_READ | HAL_VM_PROT_WRITE,
                    NULL) != -EACCES) {
        rc = -EACCES;
        goto cleanup;
    }

    rc = nos_cap_alloc(nos_cell_cap_table(sup_cell), NOS_CAP_VMO, weight_vmo,
                       NOS_CAP_RIGHT_ALL, 0U, &sup_vmo_h);
    if (rc != 0) {
        goto cleanup;
    }

    child_cell = nos_cell_create(sup_cell, &child_b);
    if (child_cell == NULL) {
        rc = -ENOMEM;
        goto cleanup;
    }
    /* 验证叶子 Child Cell (max_child_cells=0) 严禁再派生孙子 Cell */
    if (nos_cell_create(child_cell, &child_b) != NULL) {
        rc = -EPERM;
        goto cleanup;
    }

    rc = nos_cap_derive(nos_cell_cap_table(sup_cell), sup_vmo_h,
                        nos_cell_cap_table(child_cell),
                        NOS_CAP_RIGHT_READ | NOS_CAP_RIGHT_MAP,
                        0x2B01U, &child_vmo_h);
    if (rc != 0) {
        goto cleanup;
    }

    rc = nos_cell_map_telemetry(child_cell, SELFTEST_CELL_TELEM_VA);
    if (rc == 0) {
        rc = nos_vmo_map(weight_vmo, nos_cell_vm_space(child_cell),
                         SELFTEST_CELL_VMO_VA, 0U, PAGE_SIZE,
                         HAL_VM_PROT_USER | HAL_VM_PROT_READ, child_cell);
    }
    if (rc == 0) {
        rc = hal_vm_map(nos_cell_vm_space(child_cell), SELFTEST_CELL_CODE_VA,
                        arm64_virt_to_phys((uintptr_t)code_va), PAGE_SIZE,
                        HAL_VM_PROT_USER | HAL_VM_PROT_READ | HAL_VM_PROT_EXEC);
    }
    if (rc == 0) {
        rc = hal_vm_map(nos_cell_vm_space(child_cell), SELFTEST_CELL_STACK_VA,
                        arm64_virt_to_phys((uintptr_t)stack_va), PAGE_SIZE,
                        HAL_VM_PROT_USER | HAL_VM_PROT_READ | HAL_VM_PROT_WRITE);
    }
    if (rc != 0) {
        goto cleanup;
    }

    /* 1. 在 Child Cell EL0 运行真实测试载荷：直读 Telemetry + 读 Sealed VMO + 触发 VMO_MAP 超限 */
    memcpy((void *)code_va, arm64_el0_cell_code_start, c1_len);
    arm64_sync_icache_dcache((uintptr_t)code_va, PAGE_SIZE);
    (void)nos_completion_init(&run_done);

    t1 = task_create_ex("cell_svc", el0_cell_launcher_entry, &run_done,
                        SCHED_POLICY_RR, 5U, 0, NOS_CPU_MASK_ALL,
                        CONFIG_CORE_TASK_STACK_SIZE, 10U, el0_cell_task_cleanup);
    if (t1 == NULL) {
        rc = -ENOMEM;
        goto cleanup_comp1;
    }
    rc = nos_cell_attach_task(t1, child_cell);
    if (rc != 0) {
        task_del(t1);
        (void)nos_completion_wait(&run_done, 3000U);
        goto cleanup_comp1;
    }
    (void)task_ready(t1);
    rc = nos_completion_wait(&run_done, 5000U);
cleanup_comp1:
    (void)nos_object_close(&run_done.object);
    (void)nos_object_retire(&run_done.object);
    if (rc != 0) {
        goto cleanup;
    }

    /* 2. 替换代码页为故意写 Telemetry 只读页载荷，验证硬件 Data Abort + NOS_AUDIT_FAULT 记录 */
    memset((void *)code_va, 0, PAGE_SIZE);
    memcpy((void *)code_va, arm64_el0_cell_tamper_code_start, c2_len);
    arm64_sync_icache_dcache((uintptr_t)code_va, PAGE_SIZE);
    memset((void *)stack_va, 0, PAGE_SIZE);

    (void)nos_completion_init(&tamper_done);
    __atomic_store_n(&g_el0_fault_hits, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&g_el0_fault_ec, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&g_el0_fault_far, 0ULL, __ATOMIC_RELAXED);
    arm64_set_el0_fault_observer(selftest_fault_observer);

    t2 = task_create_ex("cell_tamper", el0_cell_launcher_entry, &tamper_done,
                        SCHED_POLICY_RR, 5U, 0, NOS_CPU_MASK_ALL,
                        CONFIG_CORE_TASK_STACK_SIZE, 10U, el0_cell_task_cleanup);
    if (t2 == NULL) {
        arm64_set_el0_fault_observer(NULL);
        rc = -ENOMEM;
        goto cleanup_comp2;
    }
    rc = nos_cell_attach_task(t2, child_cell);
    if (rc != 0) {
        arm64_set_el0_fault_observer(NULL);
        task_del(t2);
        (void)nos_completion_wait(&tamper_done, 3000U);
        goto cleanup_comp2;
    }
    (void)task_ready(t2);
    rc = nos_completion_wait(&tamper_done, 5000U);
    arm64_set_el0_fault_observer(NULL);
cleanup_comp2:
    (void)nos_object_close(&tamper_done.object);
    (void)nos_object_retire(&tamper_done.object);

    if (rc != 0 ||
        __atomic_load_n(&g_el0_fault_hits, __ATOMIC_ACQUIRE) != 1U ||
        g_el0_fault_ec != ESR_ELx_EC_DABT_LOW ||
        g_el0_fault_far != (uint64_t)SELFTEST_CELL_TELEM_VA) {
        rc = -EFAULT;
        goto cleanup;
    }

    /* 3. 验证 Child Cell 内核强制审计环包含超预算熔断与非法写异常记录 */
    audit_n = nos_cell_read_audit(child_cell, audit_recs, 16U);
    if (audit_n <= 0) {
        rc = -ENOENT;
        goto cleanup;
    }
    for (int i = 0; i < audit_n; i++) {
        if (audit_recs[i].event_type == NOS_AUDIT_BUDGET_HIT) {
            seen_budget_hit = true;
        } else if (audit_recs[i].event_type == NOS_AUDIT_FAULT &&
                   audit_recs[i].arg0 == (uint64_t)SELFTEST_CELL_TELEM_VA) {
            seen_fault = true;
        }
    }
    if (!seen_budget_hit || !seen_fault) {
        pr_err("Stage2B Child audit check failed (budget=%d fault=%d)\n",
               seen_budget_hit ? 1 : 0, seen_fault ? 1 : 0);
        rc = -EFAULT;
        goto cleanup;
    }

    /* 4. CDT 级联吊销 Supervisor 根 VMO 句柄，核实 Child 子句柄原子失效并审计 */
    rc = nos_cap_revoke_tree(nos_cell_cap_table(sup_cell), sup_vmo_h, &revoked);
    if (rc != 0 || revoked < 2U) {
        rc = -EFAULT;
        goto cleanup;
    }
    if (nos_cap_lookup(nos_cell_cap_table(child_cell), child_vmo_h,
                       NOS_CAP_VMO, 0U, NULL, NULL) != -EBADF) {
        rc = -EACCES;
        goto cleanup;
    }
    audit_n = nos_cell_read_audit(sup_cell, audit_recs, 16U);
    for (int i = 0; i < audit_n; i++) {
        if (audit_recs[i].event_type == NOS_AUDIT_CAP_REVOKE) {
            seen_cap_revoke = true;
        }
    }
    if (!seen_cap_revoke) {
        rc = -ENOENT;
        goto cleanup;
    }

    pr_info("ARM64 Stage2B selftest PASS (cdt=ok, vmo_seal_wx=ok, cell_telemetry_audit=ok)\n");
    rc = 0;

cleanup:
    if (child_cell != NULL) {
        (void)nos_cell_destroy(child_cell);
    }
    if (sup_cell != NULL) {
        (void)nos_cell_destroy(sup_cell);
    }
    if (weight_vmo != NULL) {
        (void)nos_object_close(&weight_vmo->header);
        (void)nos_object_retire(&weight_vmo->header);
    }
    if (code_va != 0U) {
        free_page(code_va);
    }
    if (stack_va != 0U) {
        free_page(stack_va);
    }
    return rc;
}
#endif

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

#ifdef CONFIG_NOS_AI_CORE
    rc = run_stage2b_selftest();
    if (rc != 0) {
        pr_err("ARM64 Stage2B selftest FAILED (%d)\n", rc);
        return rc;
    }
#endif
    return 0;
}
task_init(stage2_selftest_init);
