/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 惰性 FP / NEON / SVE 变长矢量上下文框架
 * EL1 编译采用 -mgeneral-regs-only，默认屏蔽 CPACR_EL1.FPEN|ZEN，按需陷阱激活。
 */

#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <kernel/task.h>
#include <uapi/nos/errno.h>
#include <asm/cpufeature.h>
#include <asm/sysreg.h>
#include <asm/percpu.h>
#include <asm/task.h>

static uint32_t g_arm64_sve_vl;

size_t arm64_sve_state_size(uint32_t vl)
{
    size_t raw;

    if (vl < 16U || (vl & 15U) != 0U) {
        return 0U;
    }
    raw = (size_t)vl * 34U + (size_t)(vl / 8U);
    return ((raw + 15U) & ~(size_t)15U) + 16U;
}

uint32_t arm64_sve_get_vl(void)
{
    return g_arm64_sve_vl;
}

#if defined(__aarch64__) || defined(__HOST_TEST__)

#ifdef __HOST_TEST__
extern uint64_t host_mock_read_cpacr(void);
extern void host_mock_write_cpacr(uint64_t val);
extern void host_mock_write_zcr(uint64_t val);
extern struct arm64_percpu *host_mock_this_percpu(void);
extern struct arm64_percpu *host_mock_percpu_of(uint32_t cpu);
extern struct task_struct *host_mock_current_task(void);

#define fpu_read_cpacr()        host_mock_read_cpacr()
#define fpu_write_cpacr(v)      host_mock_write_cpacr(v)
#define fpu_write_zcr(v)        host_mock_write_zcr(v)
#define fpu_this_percpu()       host_mock_this_percpu()
#define fpu_percpu_of(c)        host_mock_percpu_of(c)
#define fpu_current_task()      host_mock_current_task()
#else
static inline uint64_t fpu_read_cpacr(void)
{
    return read_sysreg(cpacr_el1);
}

static inline void fpu_write_cpacr(uint64_t val)
{
    write_sysreg(val, cpacr_el1);
    __asm__ volatile("isb" ::: "memory");
}

static inline void fpu_write_zcr(uint64_t val)
{
    write_sysreg_s(val, SYS_ZCR_EL1);
    __asm__ volatile("isb" ::: "memory");
}

#define fpu_this_percpu()       arm64_this_percpu()
#define fpu_percpu_of(c)        arm64_percpu_of(c)
#define fpu_current_task()      current
#endif

static void fpu_set_access(bool fp_on, bool sve_on)
{
    uint64_t cpacr = fpu_read_cpacr();

    cpacr &= ~(CPACR_EL1_FPEN_MASK | CPACR_EL1_ZEN_MASK);
    cpacr |= fp_on ? CPACR_EL1_FPEN_FULL : CPACR_EL1_FPEN_TRAP;
    cpacr |= sve_on ? CPACR_EL1_ZEN_FULL : CPACR_EL1_ZEN_TRAP;
    fpu_write_cpacr(cpacr);
}

static int fpu_alloc_sve_buf(struct task_struct *task, uint32_t vl)
{
    size_t need = arm64_sve_state_size(vl);
    void *raw;
    uintptr_t aligned;

    if (task == NULL || need == 0U) {
        return -EINVAL;
    }
    raw = kzalloc(need + 16U, GFP_KERNEL);
    if (raw == NULL) {
        return -ENOMEM;
    }
    aligned = ((uintptr_t)raw + 15U) & ~(uintptr_t)15U;
    task->fp_state.sve_raw = raw;
    task->fp_state.sve_state = (void *)aligned;
    task->fp_state.sve_vl = (uint16_t)vl;
    return 0;
}

void arm64_fpu_init_cpu(void)
{
    struct arm64_percpu *pcpu = fpu_this_percpu();
    const struct arm64_cpu_caps *caps = arm64_get_cpu_caps();

    if (caps != NULL && caps->has_sve) {
        fpu_set_access(true, true);
        fpu_write_zcr((uint64_t)ARM64_SVE_ZCR_LEN_512);
        uint32_t vl = arm64_sve_rdvl();
        if (vl > ARM64_SVE_MAX_VL) {
            vl = ARM64_SVE_MAX_VL;
        }
        if (vl >= 16U) {
            __atomic_store_n(&g_arm64_sve_vl, vl, __ATOMIC_RELEASE);
        }
    }

    fpu_set_access(false, false);
    if (pcpu != NULL) {
        pcpu->fpu_owner = NULL;
    }
}

void arm64_fpu_trap_handler(bool is_sve)
{
    struct arm64_percpu *pcpu = fpu_this_percpu();
    struct task_struct *task = fpu_current_task();
    const struct arm64_cpu_caps *caps = arm64_get_cpu_caps();
    uint32_t vl = __atomic_load_n(&g_arm64_sve_vl, __ATOMIC_ACQUIRE);
    bool use_sve;

    if (pcpu != NULL && pcpu->fpu_owner != NULL && pcpu->fpu_owner != task) {
        struct task_struct *old_owner = pcpu->fpu_owner;
        if (old_owner->fp_state.sve_active && old_owner->fp_state.sve_state != NULL) {
            fpu_set_access(true, true);
            arm64_sve_save(old_owner->fp_state.sve_state, old_owner->fp_state.sve_vl);
        } else {
            fpu_set_access(true, false);
            arm64_fp_save(&old_owner->fp_state);
        }
        __atomic_store_n(&old_owner->fp_state.valid, 1U, __ATOMIC_RELEASE);
        pcpu->fpu_owner = NULL;
    }

    use_sve = (caps != NULL && caps->has_sve && vl >= 16U) &&
              (is_sve || (task != NULL && task->fp_state.sve_active != 0U));

    if (use_sve && task != NULL) {
        bool had_neon_valid = (__atomic_load_n(&task->fp_state.valid, __ATOMIC_ACQUIRE) != 0U) &&
                              (task->fp_state.sve_state == NULL);
        if (had_neon_valid && pcpu != NULL && pcpu->fpu_owner == task) {
            fpu_set_access(true, false);
            arm64_fp_save(&task->fp_state);
        }

        fpu_set_access(true, true);
        if (task->fp_state.sve_state == NULL) {
            if (fpu_alloc_sve_buf(task, vl) == 0) {
                arm64_sve_zero();
                if (had_neon_valid) {
                    arm64_fp_restore(&task->fp_state);
                }
                task->fp_state.sve_active = 1U;
            } else {
                /* 极少数内存不足时退回 NEON 128-bit 保底 */
                fpu_set_access(true, false);
                if (had_neon_valid) {
                    arm64_fp_restore(&task->fp_state);
                } else {
                    arm64_fp_zero();
                }
            }
        } else {
            arm64_sve_restore(task->fp_state.sve_state, task->fp_state.sve_vl);
        }
        __atomic_store_n(&task->fp_state.valid, 1U, __ATOMIC_RELEASE);
        if (pcpu != NULL) {
            pcpu->fpu_owner = task;
        }
        return;
    }

    fpu_set_access(true, false);
    if (task != NULL) {
        if (__atomic_load_n(&task->fp_state.valid, __ATOMIC_ACQUIRE) != 0U) {
            arm64_fp_restore(&task->fp_state);
        } else {
            arm64_fp_zero();
            __atomic_store_n(&task->fp_state.valid, 1U, __ATOMIC_RELEASE);
        }
        if (pcpu != NULL) {
            pcpu->fpu_owner = task;
        }
    }
}

void arm64_fpu_on_context_switch(struct task_struct *next)
{
    struct arm64_percpu *pcpu = fpu_this_percpu();

    /*
     * 仅当当前核上确有其他任务实际使用过向量/SVE 寄存器时才切出保存，
     * 普通控制线程维持零额外向量切换开销；已激活任务切走前完成落盘以支持无 IPI 核间迁移。
     */
    if (pcpu != NULL && pcpu->fpu_owner != NULL && pcpu->fpu_owner != next) {
        struct task_struct *owner = pcpu->fpu_owner;
        if (owner->fp_state.sve_active && owner->fp_state.sve_state != NULL) {
            fpu_set_access(true, true);
            arm64_sve_save(owner->fp_state.sve_state, owner->fp_state.sve_vl);
        } else {
            fpu_set_access(true, false);
            arm64_fp_save(&owner->fp_state);
        }
        __atomic_store_n(&owner->fp_state.valid, 1U, __ATOMIC_RELEASE);
        pcpu->fpu_owner = NULL;
    }

    if (pcpu != NULL && pcpu->fpu_owner == next && next != NULL) {
        bool sve_on = (next->fp_state.sve_active != 0U && next->fp_state.sve_state != NULL);
        fpu_set_access(true, sve_on);
    } else {
        fpu_set_access(false, false);
    }
}

void arm64_fpu_task_cleanup(struct task_struct *task)
{
    uint32_t cpu;

    if (task == NULL) {
        return;
    }

    for (cpu = 0U; cpu < (uint32_t)CONFIG_NR_CPUS; cpu++) {
        struct arm64_percpu *pcpu = fpu_percpu_of(cpu);
        if (pcpu != NULL) {
            struct task_struct *expected = task;
            (void)__atomic_compare_exchange_n(&pcpu->fpu_owner, &expected, NULL, false,
                                              __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
        }
    }

    if (task->fp_state.sve_raw != NULL) {
        kfree(task->fp_state.sve_raw);
        task->fp_state.sve_raw = NULL;
    }
    task->fp_state.sve_state = NULL;
    task->fp_state.sve_active = 0U;
    task->fp_state.sve_vl = 0U;
}

#endif
