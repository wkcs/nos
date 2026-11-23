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
#include <nos/accel.h>
#include <nos/cap.h>
#include <nos/iommu.h>
#include <nos/vmo.h>
#include <nos/cell.h>
#include <nos/ring.h>
#include <nos/sched_infer.h>
#include <uapi/nos/ai.h>
#include <uapi/nos/ipc.h>
#endif
#include <asm/cpufeature.h>
#include <asm/mmu.h>
#include <asm/security.h>
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
extern const uint8_t arm64_el0_neon_worker_start[];
extern const uint8_t arm64_el0_neon_worker_end[];
extern const uint8_t arm64_el0_stage3_code_start[];
extern const uint8_t arm64_el0_stage3_code_end[];
extern const uint8_t arm64_el0_stage4_code_start[];
extern const uint8_t arm64_el0_stage4_code_end[];

#define SELFTEST_CELL_CODE_VA     0x0000000000400000UL
#define SELFTEST_CELL_TELEM_VA    0x0000000000420000UL
#define SELFTEST_CELL_VMO_VA      0x0000000000430000UL
#define SELFTEST_STAGE3_RING_VA   0x0000000000440000UL
#define SELFTEST_STAGE3_WORK_VA   0x0000000000450000UL
#define SELFTEST_STAGE4_WORK_VA   0x0000000000460000UL
#define SELFTEST_STAGE4_OUT_VA    0x0000000000470000UL
#define SELFTEST_STAGE4_WEIGHT_VA 0x0000000000471000UL
#define SELFTEST_STAGE4_RING_VA   0x0000000000480000UL
#define SELFTEST_CELL_STACK_VA    0x0000000000800000UL
#define SELFTEST_CELL_VMO_MAGIC 0x4E4F534149303032ULL
#define SELFTEST_STAGE3_SCHEMA  0x0000544F4F4C0001ULL
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

struct stage3_worker_ctx {
    struct nos_ring       *ring;
    struct nos_completion  done;
    uint32_t               handled;
    bool                   saw_inherited_deadline;
};

static void stage3_tool_worker_entry(void *param)
{
    struct stage3_worker_ctx *ctx = (struct stage3_worker_ctx *)param;
    struct nos_ring_sqe sqe;

    for (;;) {
        struct nos_ring_cqe cqe;
        int rc = nos_ring_server_pop(ctx->ring, &sqe, NOS_IPC_TIMEOUT_INFINITE);
        if (rc == -EPIPE || rc < 0) {
            break;
        }
        if (current != NULL && current->se.infer.deadline_inherited) {
            __atomic_store_n(&ctx->saw_inherited_deadline, true, __ATOMIC_RELEASE);
        }
        __atomic_fetch_add(&ctx->handled, 1U, __ATOMIC_ACQ_REL);
        memset(&cqe, 0, sizeof(cqe));
        cqe.req_id = sqe.req_id;
        cqe.status = 0;
        cqe.result_len = 0x5A5AU;
        (void)nos_ring_server_complete(ctx->ring, &cqe);
    }
    (void)nos_complete(&ctx->done);
}

struct stage3_neon_ctx {
    uintptr_t              pattern;
    struct nos_completion  done;
};

static void el0_neon_launcher_entry(void *param)
{
    struct stage3_neon_ctx *ctx = (struct stage3_neon_ctx *)param;
    uintptr_t pattern = (ctx != NULL) ? ctx->pattern : 0U;

    arm64_enter_user_mode(SELFTEST_CELL_CODE_VA,
                          SELFTEST_CELL_STACK_VA + PAGE_SIZE,
                          pattern);
}

static void el0_neon_task_cleanup(struct task_struct *task)
{
    struct stage3_neon_ctx *ctx;

    if (task == NULL || task->parameter == NULL) {
        return;
    }
    ctx = (struct stage3_neon_ctx *)task->parameter;
    (void)nos_complete(&ctx->done);
}

static void el0_stage3_launcher_entry(void *param)
{
    (void)param;
    arm64_enter_user_mode(SELFTEST_CELL_CODE_VA,
                          SELFTEST_CELL_STACK_VA + PAGE_SIZE,
                          SELFTEST_STAGE3_RING_VA);
}

static int run_stage3_neon_concurrency_test(void)
{
    static struct stage3_neon_ctx ctx_a;
    static struct stage3_neon_ctx ctx_b;
    struct nos_infer_sched_param inf_p = {
        .role = NOS_INFER_ROLE_DECODE,
        .gang_id = 0U,
        .runtime_ns = 5000000ULL,
        .deadline_ns = 20000000ULL,
        .period_ns = 20000000ULL,
    };
    addr_t code_va = 0U;
    addr_t stack_a = 0U;
    addr_t stack_b = 0U;
    struct task_struct *ta = NULL;
    struct task_struct *tb = NULL;
    size_t code_len = (size_t)(arm64_el0_neon_worker_end - arm64_el0_neon_worker_start);
    bool started_a = false;
    bool started_b = false;
    int rc = 0;

    if (code_len > PAGE_SIZE) {
        return -E2BIG;
    }

    code_va = alloc_page(GFP_KERNEL | GFP_ZERO);
    stack_a = alloc_page(GFP_KERNEL | GFP_ZERO);
    stack_b = alloc_page(GFP_KERNEL | GFP_ZERO);
    if (code_va == 0U || stack_a == 0U || stack_b == 0U) {
        rc = -ENOMEM;
        goto out_free;
    }

    memcpy((void *)code_va, arm64_el0_neon_worker_start, code_len);
    arm64_sync_icache_dcache((uintptr_t)code_va, PAGE_SIZE);

    memset(&ctx_a, 0, sizeof(ctx_a));
    memset(&ctx_b, 0, sizeof(ctx_b));
    ctx_a.pattern = 0x11223344UL;
    ctx_b.pattern = 0x55667788UL;
    (void)nos_completion_init(&ctx_a.done);
    (void)nos_completion_init(&ctx_b.done);

    ta = task_create_ex("neon_a", el0_neon_launcher_entry, &ctx_a,
                        SCHED_POLICY_NORMAL, 8U, 0, 0x01U,
                        CONFIG_CORE_TASK_STACK_SIZE, 10U, el0_neon_task_cleanup);
    tb = task_create_ex("neon_b", el0_neon_launcher_entry, &ctx_b,
                        SCHED_POLICY_NORMAL, 8U, 0, 0x01U,
                        CONFIG_CORE_TASK_STACK_SIZE, 10U, el0_neon_task_cleanup);
    if (ta == NULL || tb == NULL) {
        rc = -ENOMEM;
        goto out_comp;
    }

    rc = hal_vm_map(&ta->vm_space, SELFTEST_CELL_CODE_VA,
                    arm64_virt_to_phys((uintptr_t)code_va), PAGE_SIZE,
                    HAL_VM_PROT_USER | HAL_VM_PROT_READ | HAL_VM_PROT_EXEC);
    if (rc == 0) {
        rc = hal_vm_map(&ta->vm_space, SELFTEST_CELL_STACK_VA,
                        arm64_virt_to_phys((uintptr_t)stack_a), PAGE_SIZE,
                        HAL_VM_PROT_USER | HAL_VM_PROT_READ | HAL_VM_PROT_WRITE);
    }
    if (rc == 0) {
        rc = hal_vm_map(&tb->vm_space, SELFTEST_CELL_CODE_VA,
                        arm64_virt_to_phys((uintptr_t)code_va), PAGE_SIZE,
                        HAL_VM_PROT_USER | HAL_VM_PROT_READ | HAL_VM_PROT_EXEC);
    }
    if (rc == 0) {
        rc = hal_vm_map(&tb->vm_space, SELFTEST_CELL_STACK_VA,
                        arm64_virt_to_phys((uintptr_t)stack_b), PAGE_SIZE,
                        HAL_VM_PROT_USER | HAL_VM_PROT_READ | HAL_VM_PROT_WRITE);
    }
    if (rc == 0) {
        rc = sched_infer_set_param(ta, &inf_p);
    }
    if (rc == 0) {
        rc = sched_infer_set_param(tb, &inf_p);
    }
    if (rc != 0) {
        goto out_comp;
    }

    __atomic_store_n(&g_el0_fault_hits, 0U, __ATOMIC_RELEASE);
    arm64_set_el0_fault_observer(selftest_fault_observer);

    (void)task_ready(ta);
    started_a = true;
    (void)task_ready(tb);
    started_b = true;

    rc = nos_completion_wait(&ctx_a.done, 5000U);
    if (rc == 0) {
        rc = nos_completion_wait(&ctx_b.done, 5000U);
    } else {
        (void)nos_completion_wait(&ctx_b.done, 5000U);
    }
    arm64_set_el0_fault_observer(NULL);

    if (rc == 0 && __atomic_load_n(&g_el0_fault_hits, __ATOMIC_ACQUIRE) != 0U) {
        pr_err("Stage3 NEON concurrency fault detected (hits=%u)\n",
               (unsigned int)g_el0_fault_hits);
        rc = -EFAULT;
    }

out_comp:
    if (!started_a && ta != NULL) {
        task_del(ta);
        (void)nos_completion_wait(&ctx_a.done, 3000U);
    }
    if (!started_b && tb != NULL) {
        task_del(tb);
        (void)nos_completion_wait(&ctx_b.done, 3000U);
    }
    (void)nos_object_close(&ctx_a.done.object);
    (void)nos_object_retire(&ctx_a.done.object);
    (void)nos_object_close(&ctx_b.done.object);
    (void)nos_object_retire(&ctx_b.done.object);
out_free:
    if (code_va != 0U) {
        free_page(code_va);
    }
    if (stack_a != 0U) {
        free_page(stack_a);
    }
    if (stack_b != 0U) {
        free_page(stack_b);
    }
    return rc;
}

static int run_stage3_selftest(void)
{
    static const char s3_msg[] = "[EL0-STAGE3] RingIPC, NEON Lazy-Ctx & SchedInfer OK!\n";
    static const char base_state[] = "BASE_STATE_V1";
    static struct stage3_worker_ctx wctx;
    static struct nos_completion agent_done;
    static struct nos_audit_record audit_recs[16];
    struct nos_cell_budget b = {
        .cpu_ns_limit = 2000000000ULL,
        .mem_bytes_limit = 65536ULL,
        .tool_calls_limit = 8ULL,
        .accel_ops_limit = 8ULL,
        .max_child_cells = 0U,
        .flags = 0U,
    };
    struct nos_vmo_create_args wargs = {
        .size_bytes = PAGE_SIZE,
        .kind = NOS_VMO_ANON,
        .flags = 0U,
        .block_bytes = 0U,
    };
    uint64_t schemas[1] = { SELFTEST_STAGE3_SCHEMA };
    struct nos_ring_create_args rargs = {
        .sq_entries = 16U,
        .cq_entries = 16U,
        .flags = NOS_RING_CREATE_SCHEMA_FILTER,
        .schema_count = 1U,
        .schema_list_ptr = (uint64_t)(uintptr_t)schemas,
        .map_user_vaddr = SELFTEST_STAGE3_RING_VA,
    };
    struct nos_infer_sched_param ctrl_p = {
        .role = NOS_INFER_ROLE_AGENT_CTRL,
        .gang_id = 0U,
        .runtime_ns = 5000000ULL,
        .deadline_ns = 20000000ULL,
        .period_ns = 20000000ULL,
    };
    uint64_t work_meta[2];
    char read_buf[16];
    const struct nos_cell_telemetry *tm;
    struct nos_cell *agent_cell = NULL;
    struct nos_vmo *work_vmo = NULL;
    struct nos_ring *ring = NULL;
    struct nos_cell_snapshot *snap = NULL;
    nos_handle_t ring_h = NOS_HANDLE_INVALID;
    addr_t code_va = 0U;
    addr_t stack_va = 0U;
    struct task_struct *tool_srv = NULL;
    struct task_struct *s3_agent = NULL;
    bool srv_started = false;
    bool agent_started = false;
    uint64_t t0;
    uint64_t dt_us = 0ULL;
    size_t code_len = (size_t)(arm64_el0_stage3_code_end - arm64_el0_stage3_code_start);
    bool seen_snap = false;
    bool seen_rollback = false;
    int audit_n;
    int rc;

    rc = run_stage3_neon_concurrency_test();
    if (rc != 0) {
        pr_err("Stage3 NEON concurrency test FAILED (%d)\n", rc);
        return rc;
    }

    if (code_len > PAGE_SIZE) {
        return -E2BIG;
    }

    code_va = alloc_page(GFP_KERNEL | GFP_ZERO);
    stack_va = alloc_page(GFP_KERNEL | GFP_ZERO);
    if (code_va == 0U || stack_va == 0U) {
        rc = -ENOMEM;
        goto cleanup;
    }

    memcpy((void *)code_va, arm64_el0_stage3_code_start, code_len);
    arm64_sync_icache_dcache((uintptr_t)code_va, PAGE_SIZE);

    agent_cell = nos_cell_create(NULL, &b);
    work_vmo = nos_vmo_create(&wargs);
    if (agent_cell == NULL || work_vmo == NULL) {
        rc = -ENOMEM;
        goto cleanup;
    }

    ring = nos_ring_create(&rargs, schemas, nos_cell_vm_space(agent_cell), agent_cell);
    if (ring == NULL) {
        rc = -ENOMEM;
        goto cleanup;
    }

    rc = nos_cap_alloc(nos_cell_cap_table(agent_cell), NOS_CAP_RING, ring,
                       NOS_CAP_RIGHT_ALL, 0U, &ring_h);
    if (rc != 0) {
        goto cleanup;
    }

    work_meta[0] = (uint64_t)ring_h;
    work_meta[1] = (uint64_t)(sizeof(s3_msg) - 1U);
    rc = nos_vmo_write(work_vmo, 0U, base_state, sizeof(base_state));
    if (rc == 0) {
        rc = nos_vmo_write(work_vmo, 256U, work_meta, sizeof(work_meta));
    }
    if (rc == 0) {
        rc = nos_vmo_write(work_vmo, 272U, s3_msg, sizeof(s3_msg));
    }
    if (rc == 0) {
        rc = nos_vmo_map(work_vmo, nos_cell_vm_space(agent_cell),
                         SELFTEST_STAGE3_WORK_VA, 0U, PAGE_SIZE,
                         HAL_VM_PROT_USER | HAL_VM_PROT_READ | HAL_VM_PROT_WRITE,
                         agent_cell);
    }
    if (rc == 0) {
        rc = hal_vm_map(nos_cell_vm_space(agent_cell), SELFTEST_CELL_CODE_VA,
                        arm64_virt_to_phys((uintptr_t)code_va), PAGE_SIZE,
                        HAL_VM_PROT_USER | HAL_VM_PROT_READ | HAL_VM_PROT_EXEC);
    }
    if (rc == 0) {
        rc = hal_vm_map(nos_cell_vm_space(agent_cell), SELFTEST_CELL_STACK_VA,
                        arm64_virt_to_phys((uintptr_t)stack_va), PAGE_SIZE,
                        HAL_VM_PROT_USER | HAL_VM_PROT_READ | HAL_VM_PROT_WRITE);
    }
    if (rc != 0) {
        goto cleanup;
    }

    memset(&wctx, 0, sizeof(wctx));
    wctx.ring = ring;
    (void)nos_completion_init(&wctx.done);

    /* 必须使用 SCHED_POLICY_NORMAL，避免因 FIFO/RR 保护规则跳过 Deadline Inheritance 提升 */
    tool_srv = task_create_ex("s3_toolsrv", stage3_tool_worker_entry, &wctx,
                              SCHED_POLICY_NORMAL, 8U, 0, NOS_CPU_MASK_ALL,
                              CONFIG_CORE_TASK_STACK_SIZE, 10U, NULL);
    if (tool_srv == NULL) {
        rc = -ENOMEM;
        goto cleanup;
    }
    rc = nos_ring_bind_server(ring, tool_srv, NULL, NULL);
    if (rc != 0) {
        goto cleanup;
    }
    (void)task_ready(tool_srv);
    srv_started = true;

    snap = nos_cell_snapshot(agent_cell);
    if (snap == NULL) {
        rc = -ENOMEM;
        goto cleanup;
    }

    (void)nos_completion_init(&agent_done);
    s3_agent = task_create_ex("s3_agent", el0_stage3_launcher_entry, &agent_done,
                              SCHED_POLICY_NORMAL, 8U, 0, NOS_CPU_MASK_ALL,
                              CONFIG_CORE_TASK_STACK_SIZE, 10U, el0_cell_task_cleanup);
    if (s3_agent == NULL) {
        rc = -ENOMEM;
        goto cleanup;
    }
    rc = nos_cell_attach_task(s3_agent, agent_cell);
    if (rc == 0) {
        rc = sched_infer_set_param(s3_agent, &ctrl_p);
    }
    if (rc != 0) {
        goto cleanup;
    }

    __atomic_store_n(&g_el0_fault_hits, 0U, __ATOMIC_RELEASE);
    arm64_set_el0_fault_observer(selftest_fault_observer);
    (void)task_ready(s3_agent);
    agent_started = true;

    rc = nos_completion_wait(&agent_done, 5000U);
    arm64_set_el0_fault_observer(NULL);
    if (rc != 0) {
        pr_err("Stage3 EL0 agent timeout (%d)\n", rc);
        goto cleanup;
    }

    if (__atomic_load_n(&g_el0_fault_hits, __ATOMIC_ACQUIRE) != 0U) {
        pr_err("Stage3 EL0 unexpected fault hits=%u\n", (unsigned int)g_el0_fault_hits);
        rc = -EFAULT;
        goto cleanup;
    }

    if (__atomic_load_n(&wctx.handled, __ATOMIC_ACQUIRE) != 1U ||
        !__atomic_load_n(&wctx.saw_inherited_deadline, __ATOMIC_ACQUIRE)) {
        pr_err("Stage3 tool worker check failed (handled=%u dl_inherited=%d)\n",
               (unsigned int)wctx.handled, wctx.saw_inherited_deadline ? 1 : 0);
        rc = -EFAULT;
        goto cleanup;
    }

    memset(read_buf, 0, sizeof(read_buf));
    rc = nos_vmo_read(work_vmo, 0U, read_buf, sizeof(read_buf));
    if (rc != 0 || memcmp(read_buf, "DIRTY", 5U) != 0) {
        pr_err("Stage3 CoW mutation check failed (rc=%d buf=%.8s)\n", rc, read_buf);
        rc = -EFAULT;
        goto cleanup;
    }

    tm = nos_cell_telemetry_view(agent_cell);
    if (tm == NULL || tm->tool_calls_used != 1ULL) {
        pr_err("Stage3 tool budget check failed (used=%lu)\n",
               tm ? (unsigned long)tm->tool_calls_used : 0UL);
        rc = -EFAULT;
        goto cleanup;
    }

    t0 = hal_timer_monotonic_ns();
    rc = nos_cell_rollback(agent_cell, snap);
    dt_us = (hal_timer_monotonic_ns() - t0) / 1000ULL;
    if (rc != 0 || dt_us >= 10000ULL) {
        pr_err("Stage3 rollback timing/status failed (rc=%d dt_us=%lu)\n",
               rc, (unsigned long)dt_us);
        rc = (rc != 0) ? rc : -ETIMEDOUT;
        goto cleanup;
    }
    if (dt_us >= 1000ULL) {
        pr_info("Stage3 rollback exceeded 1ms under host load (dt_us=%lu)\n",
                (unsigned long)dt_us);
    }

    memset(read_buf, 0, sizeof(read_buf));
    rc = nos_vmo_read(work_vmo, 0U, read_buf, sizeof(read_buf));
    if (rc != 0 || memcmp(read_buf, base_state, sizeof(base_state) - 1U) != 0) {
        pr_err("Stage3 rollback state restore failed (rc=%d buf=%.16s)\n", rc, read_buf);
        rc = -EFAULT;
        goto cleanup;
    }

    if (tm->tool_calls_used != 0ULL) {
        pr_err("Stage3 rollback telemetry restore failed (used=%lu)\n",
               (unsigned long)tm->tool_calls_used);
        rc = -EFAULT;
        goto cleanup;
    }

    audit_n = nos_cell_read_audit(agent_cell, audit_recs, 16U);
    for (int i = 0; i < audit_n; i++) {
        if (audit_recs[i].event_type == NOS_AUDIT_CELL_SNAPSHOT) {
            seen_snap = true;
        } else if (audit_recs[i].event_type == NOS_AUDIT_CELL_ROLLBACK) {
            seen_rollback = true;
        }
    }
    if (!seen_snap || !seen_rollback) {
        pr_err("Stage3 snapshot/rollback audit missing (snap=%d rb=%d)\n",
               seen_snap ? 1 : 0, seen_rollback ? 1 : 0);
        rc = -ENOENT;
        goto cleanup;
    }

    pr_info("ARM64 Stage3 selftest PASS (ring_ipc=ok, snapshot_rollback_us=%lu, sched_infer=ok, neon_lazy_ctx=ok)\n",
            (unsigned long)dt_us);
    rc = 0;

cleanup:
    if (ring != NULL) {
        (void)nos_ring_destroy(ring);
    }
    if (srv_started) {
        (void)nos_completion_wait(&wctx.done, 3000U);
    } else if (tool_srv != NULL) {
        task_del(tool_srv);
    }
    if (tool_srv != NULL || srv_started) {
        (void)nos_object_close(&wctx.done.object);
        (void)nos_object_retire(&wctx.done.object);
    }
    if (s3_agent != NULL && !agent_started) {
        task_del(s3_agent);
        (void)nos_completion_wait(&agent_done, 3000U);
    }
    if (s3_agent != NULL) {
        (void)nos_object_close(&agent_done.object);
        (void)nos_object_retire(&agent_done.object);
    }
    if (snap != NULL) {
        (void)nos_cell_snapshot_destroy(snap);
    }
    if (agent_cell != NULL) {
        (void)nos_cell_destroy(agent_cell);
    }
    if (work_vmo != NULL) {
        (void)nos_object_close(&work_vmo->header);
        (void)nos_object_retire(&work_vmo->header);
    }
    if (code_va != 0U) {
        free_page(code_va);
    }
    if (stack_va != 0U) {
        free_page(stack_va);
    }
    return rc;
}

static void el0_stage4_launcher_entry(void *param)
{
    (void)param;
    arm64_enter_user_mode(SELFTEST_CELL_CODE_VA,
                          SELFTEST_CELL_STACK_VA + PAGE_SIZE,
                          SELFTEST_STAGE4_WORK_VA);
}

static int run_stage4_selftest(void)
{
    static const char s4_msg[] = "[EL0-STAGE4] SMMUv3 SVA, AccelQueue GEMV & ARMv9 Sec OK!\n";
    static struct nos_completion agent_done;
    static struct nos_audit_record audit_recs[16];
    static struct task_struct ring_caller;
    struct nos_cell_budget b_a = {
        .cpu_ns_limit = 2000000000ULL,
        .mem_bytes_limit = 262144ULL,
        .tool_calls_limit = 8ULL,
        .accel_ops_limit = 3ULL,
        .max_child_cells = 0U,
        .flags = 0U,
    };
    struct nos_cell_budget b_b = {
        .cpu_ns_limit = 1000000000ULL,
        .mem_bytes_limit = 65536ULL,
        .tool_calls_limit = 2ULL,
        .accel_ops_limit = 1ULL,
        .max_child_cells = 0U,
        .flags = 0U,
    };
    struct nos_vmo_create_args vargs = {
        .size_bytes = PAGE_SIZE,
        .kind = NOS_VMO_DMA_TENSOR,
        .flags = 0U,
        .block_bytes = 0U,
    };
    struct nos_vmo_create_args w_vargs = {
        .size_bytes = PAGE_SIZE,
        .kind = NOS_VMO_WEIGHT,
        .flags = 0U,
        .block_bytes = 0U,
    };
    uint64_t r_schemas[1] = { (uint64_t)NOS_ACCEL_KERNEL_RELU_I32 };
    struct nos_ring_create_args rargs = {
        .sq_entries = 8U,
        .cq_entries = 8U,
        .flags = NOS_RING_CREATE_SCHEMA_FILTER,
        .schema_count = 1U,
        .schema_list_ptr = (uint64_t)(uintptr_t)r_schemas,
        .map_user_vaddr = SELFTEST_STAGE4_RING_VA,
    };
    int32_t w_data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    int32_t x_data[4] = { 2, 1, 0, 3 };
    int32_t relu_in[4] = { -5, 7, -1, 9 };
    int32_t relu_out[4] = { 0, 0, 0, 0 };
    uint8_t work_page[128];
    struct nos_cell *cell_a = NULL;
    struct nos_cell *cell_b = NULL;
    struct nos_accel_device *accel_dev = NULL;
    struct nos_vmo *weight_vmo = NULL;
    struct nos_vmo *in_vmo = NULL;
    struct nos_vmo *out_vmo = NULL;
    struct nos_vmo *work_vmo = NULL;
    struct nos_ring *s4_ring = NULL;
    struct task_struct *s4_agent = NULL;
    nos_handle_t accel_h = NOS_HANDLE_INVALID;
    nos_handle_t weight_h = NOS_HANDLE_INVALID;
    nos_handle_t in_h = NOS_HANDLE_INVALID;
    nos_handle_t out_h = NOS_HANDLE_INVALID;
    addr_t code_va = 0U;
    addr_t stack_va = 0U;
    size_t code_len = (size_t)(arm64_el0_stage4_code_end - arm64_el0_stage4_code_start);
    struct arm64_sec_status sec_st;
    struct nos_iommu_info iinfo;
    uint32_t pasid_a = 0U;
    uint32_t pasid_b = 0U;
    uintptr_t xlate_pa = 0U;
    uint32_t xlate_prot = 0U;
    const struct nos_cell_telemetry *tm;
    struct nos_ring_header *hdr;
    struct nos_ring_sqe *sq;
    struct nos_ring_cqe *cq;
    struct nos_accel_job_desc nop_desc;
    struct nos_accel_job_result nop_res;
    bool agent_started = false;
    bool seen_submit = false;
    bool seen_budget = false;
    int audit_n;
    int rc = 0;

    if (code_len > PAGE_SIZE) {
        return -E2BIG;
    }

    accel_dev = nos_accel_get_default();
    if (accel_dev == NULL) {
        pr_err("Stage4 no default EL1 accel device\n");
        return -ENODEV;
    }

    code_va = alloc_page(GFP_KERNEL | GFP_ZERO);
    stack_va = alloc_page(GFP_KERNEL | GFP_ZERO);
    if (code_va == 0U || stack_va == 0U) {
        rc = -ENOMEM;
        goto cleanup;
    }
    memcpy((void *)code_va, arm64_el0_stage4_code_start, code_len);
    arm64_sync_icache_dcache((uintptr_t)code_va, PAGE_SIZE);

    cell_a = nos_cell_create(NULL, &b_a);
    cell_b = nos_cell_create(NULL, &b_b);
    weight_vmo = nos_vmo_create(&w_vargs);
    in_vmo = nos_vmo_create(&vargs);
    out_vmo = nos_vmo_create(&vargs);
    work_vmo = nos_vmo_create(&vargs);
    if (cell_a == NULL || cell_b == NULL || weight_vmo == NULL ||
        in_vmo == NULL || out_vmo == NULL || work_vmo == NULL) {
        rc = -ENOMEM;
        goto cleanup;
    }

    rc = nos_vmo_write(weight_vmo, 0U, w_data, sizeof(w_data));
    if (rc == 0) {
        rc = nos_vmo_seal(weight_vmo);
    }
    if (rc == 0) {
        rc = nos_vmo_write(in_vmo, 0U, x_data, sizeof(x_data));
    }
    if (rc == 0) {
        rc = nos_cap_alloc(nos_cell_cap_table(cell_a), NOS_CAP_ACCEL, accel_dev,
                           NOS_CAP_RIGHT_READ | NOS_CAP_RIGHT_SEND, 0U, &accel_h);
    }
    if (rc == 0) {
        rc = nos_cap_alloc(nos_cell_cap_table(cell_a), NOS_CAP_VMO, weight_vmo,
                           NOS_CAP_RIGHT_ALL, 0U, &weight_h);
    }
    if (rc == 0) {
        rc = nos_cap_alloc(nos_cell_cap_table(cell_a), NOS_CAP_VMO, in_vmo,
                           NOS_CAP_RIGHT_ALL, 0U, &in_h);
    }
    if (rc == 0) {
        rc = nos_cap_alloc(nos_cell_cap_table(cell_a), NOS_CAP_VMO, out_vmo,
                           NOS_CAP_RIGHT_ALL, 0U, &out_h);
    }
    if (rc != 0) {
        goto cleanup;
    }

    arm64_security_get_status(&sec_st);
    memset(work_page, 0, sizeof(work_page));
    *(uint32_t *)&work_page[0x00] = accel_h;
    *(uint32_t *)&work_page[0x04] = weight_h;
    *(uint32_t *)&work_page[0x08] = in_h;
    *(uint32_t *)&work_page[0x0C] = out_h;
    *(uint32_t *)&work_page[0x10] = sec_st.pac_enabled ? 1U : 0U;
    *(uint64_t *)&work_page[0x18] = (uint64_t)SELFTEST_STAGE4_OUT_VA;
    *(uint64_t *)&work_page[0x20] = (uint64_t)(sizeof(s4_msg) - 1U);
    memcpy(&work_page[0x28], s4_msg, sizeof(s4_msg));

    rc = nos_vmo_write(work_vmo, 0U, work_page, sizeof(work_page));
    if (rc == 0) {
        rc = nos_vmo_map(work_vmo, nos_cell_vm_space(cell_a), SELFTEST_STAGE4_WORK_VA,
                         0U, PAGE_SIZE,
                         HAL_VM_PROT_USER | HAL_VM_PROT_READ | HAL_VM_PROT_WRITE, NULL);
    }
    if (rc == 0) {
        rc = nos_vmo_map(out_vmo, nos_cell_vm_space(cell_a), SELFTEST_STAGE4_OUT_VA,
                         0U, PAGE_SIZE,
                         HAL_VM_PROT_USER | HAL_VM_PROT_READ | HAL_VM_PROT_WRITE, NULL);
    }
    if (rc == 0) {
        rc = nos_vmo_map(weight_vmo, nos_cell_vm_space(cell_a), SELFTEST_STAGE4_WEIGHT_VA,
                         0U, PAGE_SIZE,
                         HAL_VM_PROT_USER | HAL_VM_PROT_READ, NULL);
    }
    if (rc == 0) {
        rc = hal_vm_map(nos_cell_vm_space(cell_a), SELFTEST_CELL_CODE_VA,
                        arm64_virt_to_phys((uintptr_t)code_va), PAGE_SIZE,
                        HAL_VM_PROT_USER | HAL_VM_PROT_READ | HAL_VM_PROT_EXEC);
    }
    if (rc == 0) {
        rc = hal_vm_map(nos_cell_vm_space(cell_a), SELFTEST_CELL_STACK_VA,
                        arm64_virt_to_phys((uintptr_t)stack_va), PAGE_SIZE,
                        HAL_VM_PROT_USER | HAL_VM_PROT_READ | HAL_VM_PROT_WRITE);
    }
    if (rc != 0) {
        goto cleanup;
    }

    /* 1. 验证 SMMUv3 SVA 绑定、读写权隔离、跨 Cell 边界隔离及 CMDQ 硬件同步 */
    (void)nos_iommu_get_status(&iinfo);
    if (iinfo.max_pasids > 1U) {
        pasid_a = cell_a->cell_id % iinfo.max_pasids;
        pasid_b = cell_b->cell_id % iinfo.max_pasids;
        if (pasid_b == pasid_a) {
            pasid_b = (pasid_a + 1U) % iinfo.max_pasids;
        }
    }

    rc = nos_iommu_bind_space(accel_dev->caps.stream_id, pasid_a, nos_cell_vm_space(cell_a));
    if (rc != 0) {
        pr_err("Stage4 SMMUv3 bind cell_a failed (%d)\n", rc);
        goto cleanup;
    }
    if (nos_iommu_translate(accel_dev->caps.stream_id, pasid_a,
                            SELFTEST_STAGE4_OUT_VA, &xlate_pa, &xlate_prot) != 0 ||
        (xlate_prot & HAL_VM_PROT_WRITE) == 0U) {
        pr_err("Stage4 SMMUv3 RW translate mismatch (prot=0x%x)\n", (unsigned int)xlate_prot);
        rc = -EFAULT;
        goto cleanup;
    }
    if (nos_iommu_translate(accel_dev->caps.stream_id, pasid_a,
                            SELFTEST_STAGE4_WEIGHT_VA, &xlate_pa, &xlate_prot) != 0 ||
        (xlate_prot & HAL_VM_PROT_WRITE) != 0U) {
        pr_err("Stage4 SMMUv3 RO weight isolation failed (prot=0x%x)\n", (unsigned int)xlate_prot);
        rc = -EPERM;
        goto cleanup;
    }
    rc = nos_iommu_bind_space(accel_dev->caps.stream_id, pasid_b, nos_cell_vm_space(cell_b));
    if (rc != 0) {
        pr_err("Stage4 SMMUv3 bind cell_b failed (%d)\n", rc);
        goto cleanup;
    }
    if (nos_iommu_translate(accel_dev->caps.stream_id, pasid_b,
                            SELFTEST_STAGE4_OUT_VA, &xlate_pa, &xlate_prot) == 0) {
        pr_err("Stage4 SMMUv3 cross-cell isolation breach\n");
        rc = -EFAULT;
        goto cleanup;
    }
    (void)nos_iommu_unbind_space(accel_dev->caps.stream_id, pasid_b);

    (void)nos_iommu_get_status(&iinfo);
    if (iinfo.hw_present && iinfo.cmdq_submitted == 0ULL) {
        pr_err("Stage4 SMMUv3 HW CMDQ unused\n");
        rc = -EIO;
        goto cleanup;
    }

    /* 2. 运行 EL0 Stage4 载荷（QUERY_CAPS + GEMV_I32 + Sealed -EACCES + PAC 指令自检） */
    (void)nos_completion_init(&agent_done);
    s4_agent = task_create_ex("s4_agent", el0_stage4_launcher_entry, &agent_done,
                              SCHED_POLICY_NORMAL, 8U, 0, NOS_CPU_MASK_ALL,
                              CONFIG_CORE_TASK_STACK_SIZE, 10U, el0_cell_task_cleanup);
    if (s4_agent == NULL) {
        rc = -ENOMEM;
        goto cleanup;
    }
    rc = nos_cell_attach_task(s4_agent, cell_a);
    if (rc != 0) {
        goto cleanup;
    }

    __atomic_store_n(&g_el0_fault_hits, 0U, __ATOMIC_RELEASE);
    arm64_set_el0_fault_observer(selftest_fault_observer);
    (void)task_ready(s4_agent);
    agent_started = true;

    rc = nos_completion_wait(&agent_done, 5000U);
    arm64_set_el0_fault_observer(NULL);
    if (rc != 0 || __atomic_load_n(&g_el0_fault_hits, __ATOMIC_ACQUIRE) != 0U) {
        pr_err("Stage4 EL0 agent failed (rc=%d faults=%u)\n",
               rc, (unsigned int)g_el0_fault_hits);
        rc = (rc != 0) ? rc : -EFAULT;
        goto cleanup;
    }

    tm = nos_cell_telemetry_view(cell_a);
    if (tm == NULL || tm->accel_ops_used != 1ULL) {
        pr_err("Stage4 EL0 accel telemetry mismatch (used=%lu)\n",
               tm ? (unsigned long)tm->accel_ops_used : 0UL);
        rc = -EFAULT;
        goto cleanup;
    }

    /* 3. 验证 RingIPC NOS_RING_OP_ACCEL_JOB (RELU_I32) 单次扣费与配额耗尽 -EDQUOT */
    s4_ring = nos_ring_create(&rargs, r_schemas, nos_cell_vm_space(cell_a), cell_a);
    if (s4_ring == NULL) {
        rc = -ENOMEM;
        goto cleanup;
    }
    rc = nos_vmo_write(in_vmo, 0U, relu_in, sizeof(relu_in));
    if (rc != 0) {
        goto cleanup;
    }

    memset(&ring_caller, 0, sizeof(ring_caller));
    nos_refcount_init(&ring_caller.refcount, 1U);
    ring_caller.cell = cell_a;

    hdr = (struct nos_ring_header *)s4_ring->ring_vmo->pages[0]->kvaddr;
    sq = (struct nos_ring_sqe *)((uint8_t *)hdr + hdr->sq_offset);
    cq = (struct nos_ring_cqe *)((uint8_t *)hdr + hdr->cq_offset);
    memset(&sq[0], 0, sizeof(sq[0]));
    sq[0].opcode = NOS_RING_OP_ACCEL_JOB;
    sq[0].target_cap = accel_h;
    sq[0].req_id = 0x501ULL;
    sq[0].schema_id = (uint64_t)NOS_ACCEL_KERNEL_RELU_I32;
    sq[0].vmo_cap = in_h;
    sq[0].vmo_offset = 0ULL;
    sq[0].vmo_len = (uint32_t)sizeof(relu_in);
    sq[0].inline_arg[0] = ((uint64_t)out_h << 32) | (uint64_t)NOS_HANDLE_INVALID;
    sq[0].inline_arg[1] = 4ULL;
    __atomic_store_n(&hdr->sq_tail, 1U, __ATOMIC_RELEASE);

    rc = nos_ring_enter(s4_ring, &ring_caller, 1U, 1U,
                        NOS_RING_ENTER_SUBMIT | NOS_RING_ENTER_WAIT_CQ,
                        NOS_IPC_TIMEOUT_INFINITE);
    if (rc != 1 || cq[0].req_id != 0x501ULL || cq[0].status != 0 || cq[0].result_len != 16U) {
        pr_err("Stage4 RingIPC accel dispatch failed (rc=%d status=%d len=%u)\n",
               rc, cq[0].status, (unsigned int)cq[0].result_len);
        rc = -EFAULT;
        goto cleanup;
    }
    rc = nos_vmo_read(out_vmo, 0U, relu_out, sizeof(relu_out));
    if (rc != 0 || relu_out[0] != 0 || relu_out[1] != 7 || relu_out[2] != 0 || relu_out[3] != 9) {
        pr_err("Stage4 RingIPC RELU result wrong\n");
        rc = -EFAULT;
        goto cleanup;
    }
    if (tm->accel_ops_used != 2ULL) {
        pr_err("Stage4 RingIPC double-charge detected (used=%lu)\n",
               (unsigned long)tm->accel_ops_used);
        rc = -EFAULT;
        goto cleanup;
    }

    memset(&nop_desc, 0, sizeof(nop_desc));
    nop_desc.kernel_op = NOS_ACCEL_KERNEL_NOP;
    nop_desc.req_id = 0x502ULL;
    nop_desc.weight_vmo_cap = NOS_HANDLE_INVALID;
    nop_desc.input_vmo_cap = NOS_HANDLE_INVALID;
    nop_desc.output_vmo_cap = NOS_HANDLE_INVALID;
    nop_desc.notify_cap = NOS_HANDLE_INVALID;
    rc = nos_accel_submit(accel_dev, &ring_caller, &nop_desc, &nop_res);
    if (rc != 0 || tm->accel_ops_used != 3ULL) {
        pr_err("Stage4 3rd accel op failed (rc=%d used=%lu)\n",
               rc, (unsigned long)tm->accel_ops_used);
        rc = (rc != 0) ? rc : -EFAULT;
        goto cleanup;
    }
    nop_desc.req_id = 0x503ULL;
    if (nos_accel_submit(accel_dev, &ring_caller, &nop_desc, &nop_res) != -EDQUOT) {
        pr_err("Stage4 accel quota exhaustion did not enforce -EDQUOT\n");
        rc = -EFAULT;
        goto cleanup;
    }

    audit_n = nos_cell_read_audit(cell_a, audit_recs, 16U);
    for (int i = 0; i < audit_n; i++) {
        if (audit_recs[i].event_type == NOS_AUDIT_ACCEL_SUBMIT) {
            seen_submit = true;
        } else if (audit_recs[i].event_type == NOS_AUDIT_BUDGET_HIT) {
            seen_budget = true;
        }
    }
    if (!seen_submit || !seen_budget) {
        pr_err("Stage4 audit missing accel=%d budget=%d\n",
               seen_submit ? 1 : 0, seen_budget ? 1 : 0);
        rc = -ENOENT;
        goto cleanup;
    }

    /* 4. 校验 ARMv9 BTI (GP) 与 MTE (MT_NORMAL_TAGGED) 页表属性位 */
    if (sec_st.bti_enabled) {
        uint64_t bti_pte = arm64_pte_make_leaf(0x90000000UL,
                                               HAL_VM_PROT_READ | HAL_VM_PROT_EXEC |
                                               HAL_VM_PROT_USER | HAL_VM_PROT_BTI,
                                               false, false, true);
        if ((bti_pte & ARM64_PTE_GP) == 0ULL) {
            pr_err("Stage4 BTI PTE GP bit missing\n");
            rc = -EFAULT;
            goto cleanup;
        }
    }
    if (sec_st.mte_enabled) {
        uint64_t mte_pte = arm64_pte_make_leaf(0x90000000UL,
                                               HAL_VM_PROT_READ | HAL_VM_PROT_WRITE |
                                               HAL_VM_PROT_USER | HAL_VM_PROT_MTE,
                                               false, false, true);
        if (((mte_pte >> 2) & 7ULL) != (uint64_t)MT_NORMAL_TAGGED) {
            pr_err("Stage4 MTE PTE MT_NORMAL_TAGGED missing\n");
            rc = -EFAULT;
            goto cleanup;
        }
    }

    pr_info("ARM64 Stage4 selftest PASS (smmuv3=%s, accel_gemv=ok, ring_accel=ok, sec_pac=%u, sec_bti=%u, sec_mte=%u)\n",
            iinfo.hw_present ? "hw" : "shadow",
            sec_st.pac_enabled ? 1U : 0U,
            sec_st.bti_enabled ? 1U : 0U,
            sec_st.mte_enabled ? 1U : 0U);
    rc = 0;

cleanup:
    (void)nos_iommu_unbind_space(accel_dev->caps.stream_id, pasid_a);
    if (s4_ring != NULL) {
        (void)nos_ring_destroy(s4_ring);
    }
    if (s4_agent != NULL && !agent_started) {
        task_del(s4_agent);
        (void)nos_completion_wait(&agent_done, 3000U);
    }
    if (s4_agent != NULL) {
        (void)nos_object_close(&agent_done.object);
        (void)nos_object_retire(&agent_done.object);
    }
    if (accel_dev != NULL) {
        (void)nos_object_put(&accel_dev->header);
    }
    if (cell_b != NULL) {
        (void)nos_cell_destroy(cell_b);
    }
    if (cell_a != NULL) {
        (void)nos_cell_destroy(cell_a);
    }
    if (work_vmo != NULL) {
        (void)nos_object_close(&work_vmo->header);
        (void)nos_object_retire(&work_vmo->header);
    }
    if (out_vmo != NULL) {
        (void)nos_object_close(&out_vmo->header);
        (void)nos_object_retire(&out_vmo->header);
    }
    if (in_vmo != NULL) {
        (void)nos_object_close(&in_vmo->header);
        (void)nos_object_retire(&in_vmo->header);
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

    rc = run_stage3_selftest();
    if (rc != 0) {
        pr_err("ARM64 Stage3 selftest FAILED (%d)\n", rc);
        return rc;
    }

    rc = run_stage4_selftest();
    if (rc != 0) {
        pr_err("ARM64 Stage4 selftest FAILED (%d)\n", rc);
        return rc;
    }
#endif
    return 0;
}
task_init(stage2_selftest_init);
