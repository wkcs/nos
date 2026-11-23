/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * ARMv9 SVE 变长矢量惰性上下文切换状态机与偏移布局主机单元测试
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <kernel/task.h>
#include <asm/cpufeature.h>
#include <asm/percpu.h>
#include <asm/sysreg.h>
#include <asm/task.h>

static struct arm64_cpu_caps g_caps;
static struct arm64_percpu g_pcpus[CONFIG_NR_CPUS];
static uint32_t g_cur_cpu;
static struct task_struct *g_cur_task;

static uint64_t g_cpacr;
static uint64_t g_zcr;
static uint32_t g_hw_vl = 64U;

/* 模拟当前硬件寄存器文件与调用次数统计 */
static uint8_t  g_hw_z[64U * 32U];
static uint64_t g_fp_zero_cnt;
static uint64_t g_fp_save_cnt;
static uint64_t g_fp_restore_cnt;
static uint64_t g_sve_zero_cnt;
static uint64_t g_sve_save_cnt;
static uint64_t g_sve_restore_cnt;
static uint32_t g_live_allocs;

void *kzalloc(size_t size, gfp_t flags)
{
    (void)flags;
    g_live_allocs++;
    return calloc(1U, size);
}

void kfree(void *ptr)
{
    if (ptr != NULL) {
        assert(g_live_allocs > 0U);
        g_live_allocs--;
        free(ptr);
    }
}

const struct arm64_cpu_caps *arm64_get_cpu_caps(void)
{
    return &g_caps;
}

uint64_t host_mock_read_cpacr(void)
{
    return g_cpacr;
}

void host_mock_write_cpacr(uint64_t val)
{
    g_cpacr = val;
}

void host_mock_write_zcr(uint64_t val)
{
    g_zcr = val;
}

struct arm64_percpu *host_mock_this_percpu(void)
{
    return &g_pcpus[g_cur_cpu];
}

struct arm64_percpu *host_mock_percpu_of(uint32_t cpu)
{
    return (cpu < (uint32_t)CONFIG_NR_CPUS) ? &g_pcpus[cpu] : NULL;
}

struct task_struct *host_mock_current_task(void)
{
    return g_cur_task;
}

void arm64_fp_zero(void)
{
    g_fp_zero_cnt++;
    for (uint32_t i = 0U; i < 32U; i++) {
        memset(&g_hw_z[(size_t)i * 64U], 0, 16U);
    }
}

void arm64_fp_save(struct arm64_fp_state *state)
{
    g_fp_save_cnt++;
    for (uint32_t i = 0U; i < 32U; i++) {
        memcpy(&state->vregs[(size_t)i * 2U], &g_hw_z[(size_t)i * 64U], 16U);
    }
}

void arm64_fp_restore(const struct arm64_fp_state *state)
{
    g_fp_restore_cnt++;
    for (uint32_t i = 0U; i < 32U; i++) {
        memcpy(&g_hw_z[(size_t)i * 64U], &state->vregs[(size_t)i * 2U], 16U);
    }
}

uint32_t arm64_sve_rdvl(void)
{
    return g_hw_vl;
}

void arm64_sve_zero(void)
{
    g_sve_zero_cnt++;
    memset(g_hw_z, 0, sizeof(g_hw_z));
}

void arm64_sve_save(void *buf, uint32_t vl)
{
    g_sve_save_cnt++;
    memcpy(buf, g_hw_z, (size_t)vl * 32U);
}

void arm64_sve_restore(const void *buf, uint32_t vl)
{
    g_sve_restore_cnt++;
    memcpy(g_hw_z, buf, (size_t)vl * 32U);
}

int main(void)
{
    struct task_struct t_ctrl;
    struct task_struct t_sve_a;
    struct task_struct t_neon_up;

    /* 1. 验证不同 VL 下的 SVE 状态尺寸与 16B 对齐公式 */
    assert(arm64_sve_state_size(0U) == 0U);
    assert(arm64_sve_state_size(12U) == 0U);
    assert(arm64_sve_state_size(16U) == 576U);
    assert(arm64_sve_state_size(32U) == 1120U);
    assert(arm64_sve_state_size(64U) == 2208U);

    /* 2. 验证硬件存在 SVE (设 hardware VL=128 -> 收敛到 MAX 64B=512-bit) 的初始化 */
    memset(&g_caps, 0, sizeof(g_caps));
    memset(g_pcpus, 0, sizeof(g_pcpus));
    g_caps.has_fp = true;
    g_caps.has_asimd = true;
    g_caps.has_sve = true;
    g_hw_vl = 128U;
    g_cur_cpu = 0U;

    arm64_fpu_init_cpu();
    assert(g_zcr == (uint64_t)ARM64_SVE_ZCR_LEN_512);
    assert(arm64_sve_get_vl() == 64U);
    assert((g_cpacr & (CPACR_EL1_FPEN_MASK | CPACR_EL1_ZEN_MASK)) == 0ULL);

    /* 3. 普通控制线程 t_ctrl 从未触发向量异常：切换开销与 SVE 分配均为 0 */
    memset(&t_ctrl, 0, sizeof(t_ctrl));
    memset(&t_sve_a, 0, sizeof(t_sve_a));
    memset(&t_neon_up, 0, sizeof(t_neon_up));

    g_cur_task = &t_ctrl;
    arm64_fpu_on_context_switch(&t_ctrl);
    assert(g_live_allocs == 0U);
    assert((g_cpacr & (CPACR_EL1_FPEN_MASK | CPACR_EL1_ZEN_MASK)) == 0ULL);

    /* 4. SVE 任务 t_sve_a 首次触发 EC_SVE：分配 64B VL 缓冲区并解锁 FPEN | ZEN */
    g_cur_task = &t_sve_a;
    arm64_fpu_on_context_switch(&t_sve_a);
    arm64_fpu_trap_handler(true);
    assert(t_sve_a.fp_state.sve_active == 1U);
    assert(t_sve_a.fp_state.sve_vl == 64U);
    assert(t_sve_a.fp_state.sve_state != NULL);
    assert(((uintptr_t)t_sve_a.fp_state.sve_state & 15U) == 0U);
    assert(g_live_allocs == 1U);
    assert((g_cpacr & (CPACR_EL1_FPEN_MASK | CPACR_EL1_ZEN_MASK)) ==
           (CPACR_EL1_FPEN_FULL | CPACR_EL1_ZEN_FULL));

    /* 写入 512-bit 模式到当前硬件 Z[0..31]，切换到 t_ctrl 必须立即 SVE save 并重锁 */
    memset(g_hw_z, 0xA5, 64U * 32U);
    arm64_fpu_on_context_switch(&t_ctrl);
    assert(g_sve_save_cnt == 1U);
    assert((g_cpacr & (CPACR_EL1_FPEN_MASK | CPACR_EL1_ZEN_MASK)) == 0ULL);

    /* 5. t_neon_up 先触发 NEON 陷阱、写入低 128-bit，后就地升级到 SVE 必须完整保留低 128-bit */
    g_cur_task = &t_neon_up;
    arm64_fpu_trap_handler(false);
    assert(t_neon_up.fp_state.sve_active == 0U && t_neon_up.fp_state.sve_state == NULL);
    assert((g_cpacr & CPACR_EL1_FPEN_MASK) == CPACR_EL1_FPEN_FULL);
    assert((g_cpacr & CPACR_EL1_ZEN_MASK) == 0ULL);
    for (uint32_t i = 0U; i < 32U; i++) {
        memset(&g_hw_z[(size_t)i * 64U], 0x3C, 16U);
    }

    arm64_fpu_trap_handler(true);
    assert(t_neon_up.fp_state.sve_active == 1U && t_neon_up.fp_state.sve_state != NULL);
    assert(g_hw_z[0] == 0x3CU && g_hw_z[15] == 0x3CU);
    assert(g_hw_z[16] == 0x00U); /* 高位 SVE 向量清零无泄露 */
    assert(g_hw_z[64] == 0x3CU && g_hw_z[79] == 0x3CU);
    assert(g_hw_z[80] == 0x00U);

    /* 6. 模拟 t_neon_up 迁移到 CPU 1 并释放 t_sve_a / t_neon_up，零内存泄露且无悬垂 fpu_owner */
    arm64_fpu_on_context_switch(&t_ctrl);
    memset(g_hw_z, 0, sizeof(g_hw_z));
    g_cur_cpu = 1U;
    g_cur_task = &t_sve_a;
    arm64_fpu_on_context_switch(&t_sve_a);
    arm64_fpu_trap_handler(false); /* 同事验证 sve_active 任务经任意矢量陷阱亦恢复完整 SVE */
    assert(g_hw_z[0] == 0xA5U && g_hw_z[63] == 0xA5U);

    arm64_fpu_task_cleanup(&t_sve_a);
    arm64_fpu_task_cleanup(&t_neon_up);
    assert(g_pcpus[1].fpu_owner == NULL);
    assert(g_live_allocs == 0U);

    printf("PASS: test_arm64_sve (VL 16/32/64B layout, lazy FP->SVE upgrade, SMP cleanup)\n");
    return 0;
}
