/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 异常同步分发与 Fault 诊断（SVC 系统调用、缺页裁决、惰性向量陷阱与栈回溯）
 */

#include <kernel/kernel.h>
#include <kernel/printk.h>
#include <kernel/task.h>
#include <kernel/unwind.h>
#include <nos/syscall.h>
#include <asm/pt_regs.h>
#include <asm/sysreg.h>
#include <asm/irq.h>

extern void arm64_fpu_trap_handler(void);

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
        /* SVC #0 系统调用：允许抢占前恢复用户态传参并写入 x0 返回值 */
        long ret = nos_syscall_dispatch((unsigned long)regs->regs[8],
                                        (uintptr_t)regs->regs[0],
                                        (uintptr_t)regs->regs[1],
                                        (uintptr_t)regs->regs[2],
                                        (uintptr_t)regs->regs[3],
                                        (uintptr_t)regs->regs[4],
                                        (uintptr_t)regs->regs[5]);
        regs->regs[0] = (uint64_t)ret;
        return;
    }

    if (ec == ESR_ELx_EC_FP_ASIMD || ec == ESR_ELx_EC_SVE || ec == ESR_ELx_EC_SME) {
        arm64_fpu_trap_handler();
        return;
    }

    arm64_dump_pt_regs(regs, from_el0 ? "EL0 Sync Fault" : "EL1 Sync Panic");
    panic("Unhandled ARM64 synchronous exception");
}

void arm64_serror_handler(struct arm64_pt_regs *regs, bool from_el0)
{
    arm64_dump_pt_regs(regs, from_el0 ? "EL0 FIQ/SError" : "EL1 FIQ/SError");
    panic("Unhandled ARM64 FIQ/SError/Invalid exception");
}
