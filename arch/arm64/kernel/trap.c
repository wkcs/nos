/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 异常同步分发与 Fault 诊断（SVC 系统调用、缺页裁决、惰性向量陷阱与栈回溯）
 */

#include <kernel/kernel.h>
#include <kernel/printk.h>
#include <kernel/task.h>
#include <kernel/sched.h>
#include <kernel/unwind.h>
#include <nos/syscall.h>
#include <nos/uaccess.h>
#include <uapi/nos/syscall.h>
#include <uapi/nos/errno.h>
#include <uapi/nos/ipc.h>
#ifdef CONFIG_NOS_AI_CORE
#include <nos/cell.h>
#include <uapi/nos/ai.h>
#endif
#include <asm/pt_regs.h>
#include <asm/sysreg.h>
#include <asm/irq.h>
#include <asm/mmu.h>
#include <asm/switch.h>
#include <asm/task.h>

static arm64_el0_fault_observer_t g_el0_fault_observer;

struct arm64_ex_table_entry {
    uint64_t insn;
    uint64_t fixup;
};

extern const struct arm64_ex_table_entry __ex_table_start[];
extern const struct arm64_ex_table_entry __ex_table_end[];

void arm64_set_el0_fault_observer(arm64_el0_fault_observer_t cb)
{
    __atomic_store_n(&g_el0_fault_observer, cb, __ATOMIC_RELEASE);
}

bool arm64_user_ptr_valid(uintptr_t ptr, size_t len)
{
    uintptr_t end;

    if (ptr < PAGE_SIZE || ptr >= (uintptr_t)ARM64_USER_VA_LIMIT) {
        return false;
    }
    end = ptr + len;
    if (end < ptr || end > (uintptr_t)ARM64_USER_VA_LIMIT) {
        return false;
    }
    return true;
}

static bool arm64_user_buf_mapped(struct task_struct *task,
                                  uintptr_t ptr,
                                  size_t len,
                                  bool require_write)
{
    struct nos_vm_space *vms;
    uintptr_t cur;
    uintptr_t end;

    if (!arm64_user_ptr_valid(ptr, len)) {
        return false;
    }
    if (len == 0U) {
        return true;
    }
    vms = nos_task_active_vm_space(task);
    if (vms == NULL || !vms->initialized || vms->root_pt_phys == 0U) {
        return false;
    }

    cur = ptr & PAGE_MASK;
    end = ptr + len;
    while (cur < end) {
        uint64_t pte = 0ULL;
        if (arm64_pt_translate(vms->root_pt_phys, CONFIG_ARM64_VA_BITS,
                               cur, NULL, &pte, NULL) != 0) {
            return false;
        }
        if ((pte & ARM64_PTE_AP_EL0) == 0ULL) {
            return false;
        }
        if (require_write && (pte & ARM64_PTE_AP_RO) != 0ULL) {
            return false;
        }
        cur += PAGE_SIZE;
    }
    return true;
}

static bool arm64_validate_el0_syscall(unsigned long sysno, const struct arm64_pt_regs *regs)
{
    struct task_struct *task = current;
    uintptr_t a0 = (uintptr_t)regs->regs[0];
    uintptr_t a1 = (uintptr_t)regs->regs[1];
    uintptr_t a2 = (uintptr_t)regs->regs[2];
    uintptr_t a3 = (uintptr_t)regs->regs[3];
    uintptr_t a4 = (uintptr_t)regs->regs[4];
    uintptr_t a5 = (uintptr_t)regs->regs[5];

    switch (sysno) {
    case SYS_NOS_DEBUG_CONSOLE:
        if (a1 > 0U && !arm64_user_buf_mapped(task, a0, (size_t)a1, a2 != 0U)) {
            return false;
        }
        return true;
    case SYS_NOS_CLOCK_GET:
        if (a0 != 0U && !arm64_user_buf_mapped(task, a0, sizeof(uint64_t), true)) {
            return false;
        }
        return true;
    case SYS_NOS_THREAD_CREATE:
        if ((a0 != 0U && !arm64_user_buf_mapped(task, a0, 1U, false)) ||
            (a5 != 0U && !arm64_user_buf_mapped(task, a5, sizeof(nos_handle_t), true))) {
            return false;
        }
        return true;
    case SYS_NOS_IPC_CALL:
        if ((a1 != 0U && !arm64_user_buf_mapped(task, a1, sizeof(struct nos_ipc_msg), false)) ||
            (a2 != 0U && !arm64_user_buf_mapped(task, a2, sizeof(struct nos_ipc_msg), true))) {
            return false;
        }
        return true;
    case SYS_NOS_IPC_RECV:
        return arm64_user_buf_mapped(task, a1, sizeof(struct nos_ipc_msg), true);
    case SYS_NOS_IPC_REPLY_WAIT:
        if ((a1 != 0U && !arm64_user_buf_mapped(task, a1, sizeof(struct nos_ipc_msg), false)) ||
            (a2 != 0U && !arm64_user_buf_mapped(task, a2, sizeof(struct nos_ipc_msg), true))) {
            return false;
        }
        return true;
    case SYS_NOS_NOTIFY_WAIT:
        if (a1 != 0U && !arm64_user_buf_mapped(task, a1, sizeof(uint64_t), true)) {
            return false;
        }
        return true;
    case SYS_NOS_CAP_CONTROL:
        if (a0 == NOS_CAP_OP_INSPECT &&
            ((a2 != 0U && !arm64_user_buf_mapped(task, a2, sizeof(uint32_t), true)) ||
             (a3 != 0U && !arm64_user_buf_mapped(task, a3, sizeof(nos_cap_rights_t), true)))) {
            return false;
        }
        if (a4 != 0U && !arm64_user_buf_mapped(task, a4, sizeof(nos_handle_t), true)) {
            return false;
        }
        return true;
    case SYS_NOS_VM_MAP:
    case SYS_NOS_VM_UNMAP:
        /* 阶段 2 禁止 EL0 直接绑定裸物理页，需经 VMO 能力接口受控映射 */
        return false;
#ifdef CONFIG_NOS_AI_CORE
    case SYS_NOS_CELL_CONTROL:
        if ((a0 == NOS_CELL_OP_CREATE || a0 == NOS_CELL_OP_FORK || a0 == NOS_CELL_OP_SET_BUDGET) &&
            a2 != 0U && !arm64_user_buf_mapped(task, a2, sizeof(struct nos_cell_budget), false)) {
            return false;
        }
        if ((a0 == NOS_CELL_OP_CREATE || a0 == NOS_CELL_OP_FORK) &&
            a3 != 0U && !arm64_user_ptr_valid(a3, PAGE_SIZE)) {
            return false;
        }
        if (a0 == NOS_CELL_OP_GET_BUDGET &&
            !arm64_user_buf_mapped(task, a2, sizeof(struct nos_cell_budget), true)) {
            return false;
        }
        if (a0 == NOS_CELL_OP_READ_AUDIT) {
            if (a3 == 0U || a3 > NOS_CELL_AUDIT_RING_CAPACITY ||
                !arm64_user_buf_mapped(task, a2,
                                       (size_t)a3 * sizeof(struct nos_audit_record), true)) {
                return false;
            }
        }
        if ((a0 == NOS_CELL_OP_CREATE || a0 == NOS_CELL_OP_FORK || a0 == NOS_CELL_OP_SNAPSHOT) &&
            a4 != 0U && !arm64_user_buf_mapped(task, a4, sizeof(nos_handle_t), true)) {
            return false;
        }
        return true;
    case SYS_NOS_RING_CREATE: {
        struct nos_ring_create_args krargs;
        if (a0 == 0U || nos_copy_from_user(&krargs, a0, sizeof(krargs)) != 0) {
            return false;
        }
        if (krargs.schema_count > NOS_RING_MAX_SCHEMAS) {
            return false;
        }
        if (krargs.schema_count > 0U &&
            !arm64_user_buf_mapped(task,
                                   (uintptr_t)krargs.schema_list_ptr,
                                   (size_t)krargs.schema_count * sizeof(uint64_t),
                                   false)) {
            return false;
        }
        if (krargs.map_user_vaddr != 0ULL &&
            !arm64_user_ptr_valid((uintptr_t)krargs.map_user_vaddr, PAGE_SIZE)) {
            return false;
        }
        if (a1 != 0U && !arm64_user_buf_mapped(task, a1, sizeof(nos_handle_t), true)) {
            return false;
        }
        return true;
    }
    case SYS_NOS_RING_ENTER: {
        uint32_t rflags = (uint32_t)a3;
        if ((rflags & NOS_RING_ENTER_COMPLETE) != 0U) {
            if (a4 == 0U || !arm64_user_buf_mapped(task, a4, sizeof(struct nos_ring_cqe), false)) {
                return false;
            }
        } else if ((rflags & NOS_RING_ENTER_SERVER_POP) != 0U) {
            uint32_t max_pop = (a1 > 0U && a1 <= NOS_RING_DEFAULT_ENTRIES)
                                   ? (uint32_t)a1 : 1U;
            if (a4 == 0U || !arm64_user_buf_mapped(
                                task, a4, (size_t)max_pop * sizeof(struct nos_ring_sqe), true)) {
                return false;
            }
        }
        return true;
    }
    case SYS_NOS_SCHED_INFER:
        if (a0 == NOS_SCHED_OP_SET_INFER &&
            (a2 == 0U || !arm64_user_buf_mapped(task, a2, sizeof(struct nos_infer_sched_param), false))) {
            return false;
        }
        if (a0 == NOS_SCHED_OP_QUERY_TOPO &&
            (a2 == 0U || !arm64_user_buf_mapped(task, a2, sizeof(struct nos_cpu_topo_info), true))) {
            return false;
        }
        return true;
    case SYS_NOS_ACCEL_SUBMIT:
        if (a0 == NOS_ACCEL_OP_QUERY_CAPS &&
            (a2 == 0U || !arm64_user_buf_mapped(task, a2, sizeof(struct nos_accel_caps), true))) {
            return false;
        }
        if (a0 == NOS_ACCEL_OP_SUBMIT) {
            if (a2 == 0U || !arm64_user_buf_mapped(task, a2, sizeof(struct nos_accel_job_desc), false)) {
                return false;
            }
            if (a3 != 0U && !arm64_user_buf_mapped(task, a3, sizeof(struct nos_accel_job_result), true)) {
                return false;
            }
        }
        if (a0 == NOS_ACCEL_OP_WAIT &&
            a3 != 0U && !arm64_user_buf_mapped(task, a3, sizeof(struct nos_accel_job_result), true)) {
            return false;
        }
        return true;
    case SYS_NOS_VMO_CREATE:
        if (a0 == 0U || !arm64_user_buf_mapped(task, a0, sizeof(struct nos_vmo_create_args), false)) {
            return false;
        }
        if (a1 != 0U && !arm64_user_buf_mapped(task, a1, sizeof(nos_handle_t), true)) {
            return false;
        }
        return true;
    case SYS_NOS_VMO_CONTROL:
        if (a0 == NOS_VMO_OP_CLONE_COW &&
            a2 != 0U && !arm64_user_buf_mapped(task, a2, sizeof(nos_handle_t), true)) {
            return false;
        }
        if (a0 == NOS_VMO_OP_CLONE_KV_PREFIX &&
            a3 != 0U && !arm64_user_buf_mapped(task, a3, sizeof(nos_handle_t), true)) {
            return false;
        }
        if ((a0 == NOS_VMO_OP_MAP || a0 == NOS_VMO_OP_UNMAP) &&
            a4 > 0U && !arm64_user_ptr_valid(a3, (size_t)a4)) {
            return false;
        }
        return true;
    case SYS_NOS_CAP_REVOKE_TREE:
        if (a1 != 0U && !arm64_user_buf_mapped(task, a1, sizeof(uint32_t), true)) {
            return false;
        }
        return true;
#endif
    default:
        return true;
    }
}

static const char *arm64_ec_name(uint32_t ec)
{
    switch (ec) {
    case ESR_ELx_EC_UNKNOWN:  return "Unknown";
    case ESR_ELx_EC_FP_ASIMD: return "FP/ASIMD Trap";
    case ESR_ELx_EC_PAC:      return "PAC Auth Failure";
    case ESR_ELx_EC_BTI:      return "BTI Branch Target";
    case ESR_ELx_EC_SVC64:    return "SVC64 Syscall";
    case ESR_ELx_EC_SVE:      return "SVE Trap";
    case ESR_ELx_EC_SME:      return "SME Trap";
    case ESR_ELx_EC_IABT_LOW: return "Instr Abort (EL0)";
    case ESR_ELx_EC_IABT_CUR: return "Instr Abort (EL1)";
    case ESR_ELx_EC_PC_ALIGN: return "PC Alignment Fault";
    case ESR_ELx_EC_DABT_LOW: return "Data Abort (EL0)";
    case ESR_ELx_EC_DABT_CUR: return "Data Abort (EL1)";
    case ESR_ELx_EC_SP_ALIGN: return "SP Alignment Fault";
    case ESR_ELx_EC_BRK64:    return "BRK64 Breakpoint";
    default:                  return "Other Exception";
    }
}

static void arm64_dump_pt_regs(const struct arm64_pt_regs *regs, const char *tag)
{
    extern char __kernel_init_stack_bottom[];
    extern char __kernel_init_stack_top[];
    uint32_t ec = ESR_ELx_EC(regs->esr);
    uintptr_t stack_low = (uintptr_t)__kernel_init_stack_bottom;
    uintptr_t stack_high = (uintptr_t)__kernel_init_stack_top;

    if (current != NULL && current->stack != NULL && current->stack_size > 0U) {
        stack_high = (uintptr_t)current->stack + sizeof(addr_t);
        stack_low = stack_high - (uintptr_t)current->stack_size;
    }

    pr_fatal("\n=== ARM64 %s: %s (EC=0x%02x ISS=0x%06x) ===\n",
             tag, arm64_ec_name(ec), ec, ESR_ELx_ISS(regs->esr));
    pr_fatal("ELR_EL1=0x%016lx SPSR_EL1=0x%016lx FAR_EL1=0x%016lx\n",
             (unsigned long)regs->pc, (unsigned long)regs->pstate, (unsigned long)regs->far);
    pr_fatal("SP=0x%016lx FP(x29)=0x%016lx LR(x30)=0x%016lx\n",
             (unsigned long)regs->sp, (unsigned long)regs->regs[29], (unsigned long)regs->regs[30]);

    for (uint32_t i = 0U; i < 30U; i += 2U) {
        pr_fatal("  x%02u=0x%016lx  x%02u=0x%016lx\n",
                 i, (unsigned long)regs->regs[i],
                 i + 1U, (unsigned long)regs->regs[i + 1U]);
    }
    dump_trap_backtrace((uintptr_t)regs->sp, (uintptr_t)regs->pc,
                        (uintptr_t)regs->regs[30], (uintptr_t)regs->regs[29],
                        stack_low, stack_high, NULL);
}

void arm64_sync_handler(struct arm64_pt_regs *regs, bool from_el0)
{
    uint32_t ec = ESR_ELx_EC(regs->esr);

    if (ec == ESR_ELx_EC_SVC64) {
        long ret;
        unsigned long sysno = (unsigned long)regs->regs[8];
        struct task_struct *t = current;
        uint8_t prev_el0 = 0U;

        if (t != NULL) {
            prev_el0 = t->arch_in_el0_syscall;
            t->arch_in_el0_syscall = from_el0 ? 1U : 0U;
        }

        /* 允许在系统调用期间响应时钟与外设中断 */
        arch_irq_enable_global();
        if (from_el0 && !arm64_validate_el0_syscall(sysno, regs)) {
            if (t != NULL) {
                t->arch_in_el0_syscall = prev_el0;
            }
            regs->regs[0] = (uint64_t)(long)-EFAULT;
            return;
        }

        ret = nos_syscall_dispatch(sysno,
                                   (uintptr_t)regs->regs[0],
                                   (uintptr_t)regs->regs[1],
                                   (uintptr_t)regs->regs[2],
                                   (uintptr_t)regs->regs[3],
                                   (uintptr_t)regs->regs[4],
                                   (uintptr_t)regs->regs[5]);
        if (t != NULL) {
            t->arch_in_el0_syscall = prev_el0;
        }
        regs->regs[0] = (uint64_t)ret;
        return;
    }

    if (ec == ESR_ELx_EC_FP_ASIMD || ec == ESR_ELx_EC_SVE || ec == ESR_ELx_EC_SME) {
        arm64_fpu_trap_handler(ec == ESR_ELx_EC_SVE);
        return;
    }

    if (from_el0) {
        arm64_el0_fault_observer_t obs = __atomic_load_n(&g_el0_fault_observer, __ATOMIC_ACQUIRE);
        struct task_struct *fault_task = current;

#ifdef CONFIG_NOS_AI_CORE
        /* 硬件页故障快路：透明处理 Permission Fault CoW 分裂与 LAZY_ALLOC/RECOMPUTABLE 按需缺页 */
        if (ec == ESR_ELx_EC_DABT_LOW &&
            fault_task != NULL &&
            fault_task->cell != NULL) {
            uint32_t iss = ESR_ELx_ISS(regs->esr);
            bool is_write = (iss & (1U << 6)) != 0U;
            uint32_t dfsc = iss & 0x3FU;
            if (nos_cell_handle_page_fault(fault_task->cell, (uintptr_t)regs->far,
                                           is_write, dfsc) == 0) {
                return;
            }
        }
#endif

        pr_err("[EL0-FAULT] task=%s ec=%s(0x%02x) iss=0x%06x far=0x%016lx elr=0x%016lx\n",
               (fault_task && fault_task->name) ? fault_task->name : "unknown",
               arm64_ec_name(ec), ec, ESR_ELx_ISS(regs->esr),
               (unsigned long)regs->far, (unsigned long)regs->pc);
        if (ec == ESR_ELx_EC_BRK64) {
            arm64_dump_pt_regs(regs, "EL0 BRK64");
        }
#ifdef CONFIG_NOS_AI_CORE
        if (fault_task != NULL && fault_task->cell != NULL) {
            nos_cell_record_audit(fault_task->cell,
                                  NOS_AUDIT_FAULT,
                                  0ULL,
                                  0U,
                                  -EFAULT,
                                  (uint64_t)regs->far,
                                  (((uint64_t)ec << 32) | (uint64_t)ESR_ELx_ISS(regs->esr)));
        }
#endif
        if (obs != NULL) {
            obs(ec, regs->far, regs->pc);
        }
        arch_irq_enable_global();
        if (fault_task != NULL) {
            task_del(fault_task);
        }
        while (1) {
            switch_task();
        }
    }

    if (!from_el0 && ec == ESR_ELx_EC_DABT_CUR) {
        for (const struct arm64_ex_table_entry *ex = __ex_table_start;
             ex < __ex_table_end;
             ex++) {
            if (ex->insn == regs->pc) {
                regs->pc = ex->fixup;
                return;
            }
        }
    }

    arm64_dump_pt_regs(regs, "EL1 Sync Panic");
    panic("Unhandled ARM64 synchronous exception");
}

void arm64_serror_handler(struct arm64_pt_regs *regs, bool from_el0)
{
    arm64_dump_pt_regs(regs, from_el0 ? "EL0 FIQ/SError" : "EL1 FIQ/SError");
    panic("Unhandled ARM64 FIQ/SError/Invalid exception");
}
