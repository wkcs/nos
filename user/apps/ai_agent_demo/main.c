/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * 独立编译的 AArch64 EL0 C Agent 示例 ELF (/bin/ai_agent.elf)
 * 验证：argv 传递、BootInfo + 只读 Telemetry 直读、LAZY_ALLOC ulibc 堆、
 *       CPU 算力拓扑查询、VMO 分配/映射/DMA 同步/Seal 不可变封印、
 *       SYS_NOS_ACCEL_SUBMIT 同步+ASYNC+WAIT+CANCEL 精确计算与 Sealed 输出 -EACCES 拦截。
 */

#include <nos/ulibc.h>

#define DEMO_VA_WEIGHT  0x0000000000500000UL
#define DEMO_VA_INPUT   0x0000000000501000UL
#define DEMO_VA_OUTPUT  0x0000000000502000UL

static volatile uint32_t g_bss_probe;
static uint32_t g_data_magic = 0x4E4F5345U;

int nos_user_main(const struct nos_user_boot_info *boot)
{
    const volatile struct nos_cell_telemetry *telem;
    struct nos_cpu_topo_info topo;
    struct nos_accel_caps acaps;
    struct nos_accel_job_desc desc;
    struct nos_accel_job_result res;
    nos_handle_t w_cap = NOS_HANDLE_INVALID;
    nos_handle_t x_cap = NOS_HANDLE_INVALID;
    nos_handle_t y_cap = NOS_HANDLE_INVALID;
    volatile int32_t *w_ptr;
    volatile int32_t *x_ptr;
    volatile int32_t *y_ptr;
    const char *const *argv;
    uint64_t *heap_buf;
    int32_t weights[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    int32_t inputs[4] = { 2, 1, 0, 3 };
    int rc;

    /* 1. 验证 ELF 加载器 .data 初值、.bss 零填充、BootInfo、argv 与只读 Telemetry 页 */
    if (g_bss_probe != 0U || g_data_magic != 0x4E4F5345U) {
        return 10;
    }
    if (boot == NULL || boot->magic != NOS_USER_BOOT_MAGIC ||
        boot->accel_cap == NOS_HANDLE_INVALID || boot->telemetry_va == 0ULL ||
        boot->argc < 1U || boot->argv_va == 0ULL) {
        return 11;
    }

    argv = (const char *const *)(uintptr_t)boot->argv_va;
    if (argv[0] == NULL || ustrlen(argv[0]) == 0U || argv[boot->argc] != NULL) {
        return 22;
    }

    telem = (const volatile struct nos_cell_telemetry *)(uintptr_t)boot->telemetry_va;
    if (telem->magic != NOS_CELL_TELEMETRY_MAGIC || telem->limit.accel_ops_limit < 3ULL) {
        return 12;
    }

    /* 1.5 验证基于 LAZY_ALLOC VMO 缺页按需提交的 ulibc umalloc/ucalloc/ufree */
    heap_buf = (uint64_t *)umalloc(256U);
    if (heap_buf == NULL) {
        return 23;
    }
    heap_buf[0] = 0x1122334455667788ULL;
    heap_buf[31] = 0xCAFEBABE12345678ULL;
    ufree(heap_buf);
    heap_buf = (uint64_t *)ucalloc(16U, sizeof(uint64_t));
    if (heap_buf == NULL || heap_buf[0] != 0ULL || heap_buf[15] != 0ULL) {
        return 24;
    }
    ufree(heap_buf);

    /* 2. 查询硬件异构核算力拓扑 */
    umemset(&topo, 0, sizeof(topo));
    rc = nos_user_sched_query_topo(&topo);
    if (rc != 0 || topo.nr_cpus < 1U || topo.online_mask == 0U) {
        return 13;
    }

    /* 3. 创建并映射 W/X/Y 三组 4KB VMO；写入权重后 SYNC_DMA + 永久封印再只读挂回 */
    if (nos_user_vmo_create(4096ULL, NOS_VMO_DMA_TENSOR, 0U, &w_cap) != 0 ||
        nos_user_vmo_create(4096ULL, NOS_VMO_DMA_TENSOR, 0U, &x_cap) != 0 ||
        nos_user_vmo_create(4096ULL, NOS_VMO_DMA_TENSOR, 0U, &y_cap) != 0) {
        return 14;
    }

    if (nos_user_vmo_map(w_cap, NOS_HANDLE_INVALID, DEMO_VA_WEIGHT, 4096U,
                         NOS_USER_PROT_READ | NOS_USER_PROT_WRITE) != 0 ||
        nos_user_vmo_map(x_cap, NOS_HANDLE_INVALID, DEMO_VA_INPUT, 4096U,
                         NOS_USER_PROT_READ | NOS_USER_PROT_WRITE) != 0 ||
        nos_user_vmo_map(y_cap, NOS_HANDLE_INVALID, DEMO_VA_OUTPUT, 4096U,
                         NOS_USER_PROT_READ | NOS_USER_PROT_WRITE) != 0) {
        return 15;
    }

    w_ptr = (volatile int32_t *)(uintptr_t)DEMO_VA_WEIGHT;
    x_ptr = (volatile int32_t *)(uintptr_t)DEMO_VA_INPUT;
    y_ptr = (volatile int32_t *)(uintptr_t)DEMO_VA_OUTPUT;
    for (uint32_t i = 0U; i < 8U; i++) {
        w_ptr[i] = weights[i];
    }
    for (uint32_t i = 0U; i < 4U; i++) {
        x_ptr[i] = inputs[i];
    }
    y_ptr[0] = 0;
    y_ptr[1] = 0;

    if (nos_user_vmo_sync_dma(w_cap, 0U, 4096U, NOS_DMA_TO_DEVICE) != 0 ||
        nos_user_vmo_sync_dma(x_cap, 0U, 4096U, NOS_DMA_TO_DEVICE) != 0) {
        return 25;
    }

    if (nos_user_vmo_unmap(w_cap, NOS_HANDLE_INVALID, DEMO_VA_WEIGHT, 4096U) != 0 ||
        nos_user_vmo_seal(w_cap) != 0 ||
        nos_user_vmo_map(w_cap, NOS_HANDLE_INVALID, DEMO_VA_WEIGHT, 4096U,
                         NOS_USER_PROT_READ) != 0) {
        return 16;
    }

    /* 4. 查询 EL1 AccelQueue 能力并提交 GEMV_I32 ([[1,2,3,4],[5,6,7,8]] * [2,1,0,3]) */
    umemset(&acaps, 0, sizeof(acaps));
    if (nos_user_accel_query_caps(boot->accel_cap, &acaps) != 0 ||
        acaps.accel_type != NOS_ACCEL_TYPE_VIRTIO) {
        return 17;
    }

    umemset(&desc, 0, sizeof(desc));
    umemset(&res, 0, sizeof(res));
    desc.kernel_op = NOS_ACCEL_KERNEL_GEMV_I32;
    desc.req_id = 0x5001ULL;
    desc.weight_vmo_cap = w_cap;
    desc.input_vmo_cap = x_cap;
    desc.output_vmo_cap = y_cap;
    desc.notify_cap = NOS_HANDLE_INVALID;
    desc.dim_m = 2U;
    desc.dim_n = 1U;
    desc.dim_k = 4U;
    desc.deadline_ns = 50000000ULL;

    rc = nos_user_accel_submit(boot->accel_cap, &desc, &res);
    if (rc != 0 || res.status != 0 || res.output_bytes != 8U) {
        return 18;
    }
    if (y_ptr[0] != 16 || y_ptr[1] != 40) {
        return 19;
    }

    /* 4.5 验证 EL0 ASYNC + WAIT 与 ASYNC + CANCEL 完整加速器闭环 */
    y_ptr[0] = 0;
    y_ptr[1] = 0;
    desc.flags = NOS_ACCEL_FLAG_ASYNC;
    desc.req_id = 0x5003ULL;
    umemset(&res, 0, sizeof(res));
    rc = nos_user_accel_submit(boot->accel_cap, &desc, &res);
    if (rc != 0 || res.status != -EINPROGRESS) {
        return 26;
    }
    umemset(&res, 0, sizeof(res));
    rc = nos_user_accel_wait(boot->accel_cap, 0x5003ULL, &res, 2000U);
    if (rc != 0 || res.status != 0 || res.output_bytes != 8U ||
        y_ptr[0] != 16 || y_ptr[1] != 40) {
        return 27;
    }

    {
        struct nos_accel_job_desc cancel_desc;
        umemset(&cancel_desc, 0, sizeof(cancel_desc));
        cancel_desc.kernel_op = NOS_ACCEL_KERNEL_NOP;
        cancel_desc.flags = NOS_ACCEL_FLAG_ASYNC;
        cancel_desc.req_id = 0x5004ULL;
        cancel_desc.weight_vmo_cap = NOS_HANDLE_INVALID;
        cancel_desc.input_vmo_cap = NOS_HANDLE_INVALID;
        cancel_desc.output_vmo_cap = NOS_HANDLE_INVALID;
        cancel_desc.notify_cap = NOS_HANDLE_INVALID;
        if (nos_user_accel_submit(boot->accel_cap, &cancel_desc, &res) != 0) {
            return 28;
        }
        (void)nos_user_accel_cancel(boot->accel_cap, 0x5004ULL);
        rc = nos_user_accel_wait(boot->accel_cap, 0x5004ULL, &res, 1000U);
        if (rc != 0 && rc != -ECANCELED) {
            return 29;
        }
    }

    /* 5. 负向验证：尝试把 Sealed 只读权重 VMO 作为输出靶区提交必须被内核拦截 -EACCES (-13) */
    desc.flags = 0U;
    desc.req_id = 0x5002ULL;
    desc.output_vmo_cap = w_cap;
    if (nos_user_accel_submit(boot->accel_cap, &desc, &res) != -NOS_EACCES) {
        return 20;
    }

    if (telem->accel_ops_used != 3ULL) {
        return 21;
    }

    uputs("[EL0-ELF64] C Agent Cell (libnos SDK): topo+accel GEMV=[16,40] OK!\n");
    return 0;
}
