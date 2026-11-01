/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/kernel.h>
#include <kernel/cpu.h>
#include <kernel/printk.h>
#include <kernel/task.h>
#include <kernel/kallsyms.h>
#include <kernel/unwind.h>
#include <asm/irq.h>
#include <asm/platform.h>
#include <nos/mm.h>

extern char __mm_pool_end[];
extern char _estack[];

#define ARMV7M_CFSR_REG  (*(volatile uint32_t *)0xE000ED28UL)
#define ARMV7M_HFSR_REG  (*(volatile uint32_t *)0xE000ED2CUL)
#define ARMV7M_MMFAR_REG (*(volatile uint32_t *)0xE000ED34UL)
#define ARMV7M_BFAR_REG  (*(volatile uint32_t *)0xE000ED38UL)

#if (defined ( __GNUC__ ) && defined ( __VFP_FP__ ) && !defined(__SOFTFP__))
#define USE_FPU   1
#else
#define USE_FPU   0
#endif

struct exception_stack_frame {
    addr_t r0;
    addr_t r1;
    addr_t r2;
    addr_t r3;
    addr_t r12;
    addr_t lr;
    addr_t pc;
    addr_t psr;
};

struct stack_frame {
#if USE_FPU
    uint32_t flag;
#endif
    addr_t r4;
    addr_t r5;
    addr_t r6;
    addr_t r7;
    addr_t r8;
    addr_t r9;
    addr_t r10;
    addr_t r11;

    struct exception_stack_frame exception_stack_frame;
};

struct cpu_dump_type {
    addr_t sp;
    struct stack_frame stack_frame;
};

static void reboot_to_bootloader(void)
{
    // TODO
    while(1) {}
}

addr_t *stack_init(void *task_entry, void *parameter, addr_t *stack_addr, void *task_exit)
{
    struct stack_frame *stack_frame;
    uint8_t *stk;
    uint8_t i;

    stk = (uint8_t *)stack_addr + sizeof(addr_t);
    stk -= sizeof(struct stack_frame);
    stack_frame = (struct stack_frame *)stk;

    for (i = 0; i < sizeof(struct stack_frame) / sizeof(addr_t); i++)
        ((addr_t *)stack_frame)[i] = 0x0badc0de;

    stack_frame->exception_stack_frame.r0  = (unsigned long)parameter;     /* r0 : argument */
    stack_frame->exception_stack_frame.r1  = 0;                            /* r1 */
    stack_frame->exception_stack_frame.r2  = 0;                            /* r2 */
    stack_frame->exception_stack_frame.r3  = 0;                            /* r3 */
    stack_frame->exception_stack_frame.r12 = 0;                            /* r12 */
    stack_frame->exception_stack_frame.lr  = (unsigned long)task_exit;     /* lr */
    stack_frame->exception_stack_frame.pc  = (unsigned long)task_entry;    /* entry point, pc */
    stack_frame->exception_stack_frame.psr = 0x01000000L;

#if USE_FPU
    stack_frame->flag = 0;
#endif

    return (addr_t *)stk;
}

void NMI_Handler(void)
{
    pr_fatal("%s entry\n", __func__);
    reboot_to_bootloader();
}

static void armv7m_decode_fault_regs(const char *fault_name)
{
    uint32_t cfsr = ARMV7M_CFSR_REG;
    uint32_t hfsr = ARMV7M_HFSR_REG;
    uint32_t mmfar = ARMV7M_MMFAR_REG;
    uint32_t bfar = ARMV7M_BFAR_REG;

    pr_info("Fault: %s | CFSR=0x%08lx HFSR=0x%08lx MMFAR=0x%08lx BFAR=0x%08lx\n",
            fault_name,
            (unsigned long)cfsr,
            (unsigned long)hfsr,
            (unsigned long)mmfar,
            (unsigned long)bfar);
    if (hfsr & (1UL << 30)) {
        pr_info("  HFSR: FORCED (escalated fault)\n");
    }
    if (hfsr & (1UL << 1)) {
        pr_info("  HFSR: VECTTBL (vector table read fault)\n");
    }
    if (cfsr & 0x000000FFUL) {
        pr_info("  MMFSR: %s%s%s%s%s\n",
                (cfsr & (1UL << 0)) ? "IACCVIOL " : "",
                (cfsr & (1UL << 1)) ? "DACCVIOL " : "",
                (cfsr & (1UL << 3)) ? "MUNSTKERR " : "",
                (cfsr & (1UL << 4)) ? "MSTKERR " : "",
                (cfsr & (1UL << 7)) ? "MMARVALID" : "");
    }
    if (cfsr & 0x0000FF00UL) {
        pr_info("  BFSR: %s%s%s%s%s%s\n",
                (cfsr & (1UL << 8)) ? "IBUSERR " : "",
                (cfsr & (1UL << 9)) ? "PRECISERR " : "",
                (cfsr & (1UL << 10)) ? "IMPRECISERR " : "",
                (cfsr & (1UL << 11)) ? "UNSTKERR " : "",
                (cfsr & (1UL << 12)) ? "STKERR " : "",
                (cfsr & (1UL << 15)) ? "BFARVALID" : "");
    }
    if (cfsr & 0xFFFF0000UL) {
        pr_info("  UFSR: %s%s%s%s%s%s\n",
                (cfsr & (1UL << 16)) ? "UNDEFINSTR " : "",
                (cfsr & (1UL << 17)) ? "INVSTATE " : "",
                (cfsr & (1UL << 18)) ? "INVPC " : "",
                (cfsr & (1UL << 19)) ? "NOCP " : "",
                (cfsr & (1UL << 24)) ? "UNALIGNED " : "",
                (cfsr & (1UL << 25)) ? "DIVBYZERO" : "");
    }
}

static void armv7m_dump_stack_top(uintptr_t sp, uintptr_t stack_high)
{
    const uint32_t *words = (const uint32_t *)(sp & ~(uintptr_t)3U);
    uint32_t i;

    pr_info("Stack top (sp=0x%08lx):\n", (unsigned long)sp);
    for (i = 0U; i < 8U; i += 4U) {
        uintptr_t addr = (uintptr_t)&words[i];
        if (stack_high != 0U && addr + 16U > stack_high) {
            break;
        }
        pr_info("  0x%08lx: %08lx %08lx %08lx %08lx\n",
                (unsigned long)addr,
                (unsigned long)words[i],
                (unsigned long)words[i + 1U],
                (unsigned long)words[i + 2U],
                (unsigned long)words[i + 3U]);
    }
}

void cpu_hard_fault(struct cpu_dump_type *cpu_dump_type)
{
    uintptr_t stack_low = 0U;
    uintptr_t stack_high = 0U;
    uintptr_t trap_sp = (uintptr_t)(cpu_dump_type + 1);

    pr_info("-------------------\n");
    pr_info("r0  = 0x%08lx\n", cpu_dump_type->stack_frame.exception_stack_frame.r0);
    pr_info("r1  = 0x%08lx\n", cpu_dump_type->stack_frame.exception_stack_frame.r1);
    pr_info("r2  = 0x%08lx\n", cpu_dump_type->stack_frame.exception_stack_frame.r2);
    pr_info("r3  = 0x%08lx\n", cpu_dump_type->stack_frame.exception_stack_frame.r3);
    pr_info("r4  = 0x%08lx\n", cpu_dump_type->stack_frame.r4);
    pr_info("r5  = 0x%08lx\n", cpu_dump_type->stack_frame.r5);
    pr_info("r6  = 0x%08lx\n", cpu_dump_type->stack_frame.r6);
    pr_info("r7  = 0x%08lx\n", cpu_dump_type->stack_frame.r7);
    pr_info("r8  = 0x%08lx\n", cpu_dump_type->stack_frame.r8);
    pr_info("r9  = 0x%08lx\n", cpu_dump_type->stack_frame.r9);
    pr_info("r10 = 0x%08lx\n", cpu_dump_type->stack_frame.r10);
    pr_info("r11 = 0x%08lx\n", cpu_dump_type->stack_frame.r11);
    pr_info("r12 = 0x%08lx\n", cpu_dump_type->stack_frame.exception_stack_frame.r12);
    pr_info("lr  = 0x%08lx\n", cpu_dump_type->stack_frame.exception_stack_frame.lr);
    pr_info("pc  = 0x%08lx\n", cpu_dump_type->stack_frame.exception_stack_frame.pc);
    pr_info("psr = 0x%08lx\n", cpu_dump_type->stack_frame.exception_stack_frame.psr);
    print_symbol("pc_sym = [<0x%08lx>] %s\n", cpu_dump_type->stack_frame.exception_stack_frame.pc);
    print_symbol("lr_sym = [<0x%08lx>] %s\n", cpu_dump_type->stack_frame.exception_stack_frame.lr);
    pr_info("-------------------\n");
    armv7m_decode_fault_regs("HardFault");

    struct task_struct *task = current;
    if (task != NULL) {
        pr_info("current task is %s\n", task->name ? task->name : "-");
    }
    if (cpu_dump_type->sp & (1UL << 2)) {
        if (task != NULL) {
            pr_info("cpu hard fault on task(name = %s)\n", task->name ? task->name : "-");
            task_get_stack_bounds(task, &stack_low, &stack_high);
        }
    } else {
        pr_info("cpu hard fault on handler\n");
        stack_low = (uintptr_t)__mm_pool_end;
        stack_high = (uintptr_t)_estack;
    }

    dump_trap_backtrace(trap_sp,
                        (uintptr_t)cpu_dump_type->stack_frame.exception_stack_frame.pc,
                        (uintptr_t)cpu_dump_type->stack_frame.exception_stack_frame.lr,
                        (uintptr_t)cpu_dump_type->stack_frame.r7,
                        stack_low,
                        stack_high,
                        NULL);
    armv7m_dump_stack_top(trap_sp, stack_high);
    dump_all_task();
    reboot_to_bootloader();
}

void MemManage_Handler(void)
{
    pr_fatal("%s entry\n", __func__);
    armv7m_decode_fault_regs("MemManage");
    arch_mpu_handle_fault(0U);
    dump_task_stack(current, NULL);
    dump_all_task();
    /* Go to infinite loop when Memory Manage exception occurs */
    reboot_to_bootloader();
}


void BusFault_Handler(void)
{
    pr_fatal("%s entry\n", __func__);
    armv7m_decode_fault_regs("BusFault");
    dump_task_stack(current, NULL);
    dump_all_task();
    /* Go to infinite loop when Bus Fault exception occurs */
    reboot_to_bootloader();
}

void UsageFault_Handler(void)
{
    pr_fatal("%s entry\n", __func__);
    armv7m_decode_fault_regs("UsageFault");
    dump_task_stack(current, NULL);
    dump_all_task();
    /* Go to infinite loop when Usage Fault exception occurs */
    reboot_to_bootloader();
}

void SVC_Handler(void)
{
    pr_fatal("%s entry\n", __func__);
    reboot_to_bootloader();
}

void DebugMon_Handler(void)
{
    pr_fatal("%s entry\n", __func__);
    reboot_to_bootloader();
}

addr_t interrupt_from_task, interrupt_to_task;
u32 switch_interrupt_flag;

static uint32_t nop_us_time_ns;
static uint32_t sys_tick_num_by_us;
static uint32_t sys_tick_num_by_heartbeat;
__init void asm_cpu_init(void)
{
#ifndef CONFIG_QEMU
    asm_disable_irq_save();
#endif
    SysTick_Config(SystemCoreClock * CONFIG_SYS_TICK_MS / 1000);

    sys_tick_num_by_us = SystemCoreClock / 1000000;
    sys_tick_num_by_heartbeat = SystemCoreClock * CONFIG_SYS_TICK_MS / 1000;
    nop_us_time_ns = (10000000000 / SystemCoreClock + 5) / 10;

    interrupt_from_task = 0;
    interrupt_to_task = 0;
    switch_interrupt_flag = 0;
}

void asm_cpu_delay_ns(uint32_t ns)
{
    register uint32_t ticks;
    register uint32_t told, tnow;
    register uint32_t tcnt = 0;
    register uint32_t reload = SysTick->LOAD;

    if (SystemCoreClock >= 1000000000) {
        ticks = ns * SystemCoreClock /  1000000000;
    } else {
        if (ns < nop_us_time_ns * 5)
            return;
        tcnt = ns / nop_us_time_ns / 4;
        while (tcnt > 0) {
            tcnt--;
        }
        return;
    }
    tcnt = 0;

    told = SysTick->VAL;
    while (1) {
        tnow = SysTick->VAL;
        if (tnow != told) {
            if (tnow < told)
                tcnt += told - tnow;
            else
                tcnt += reload - tnow + told;
            told = tnow;
            if (tcnt >= ticks) {
                break;
            }
        }
    }
}

void asm_cpu_delay_us(uint32_t us)
{
    register uint32_t ticks;
    register uint32_t told, tnow;
    register uint32_t tcnt = 0;
    register uint32_t reload = SysTick->LOAD;

    ticks = us * sys_tick_num_by_us;
    tcnt = 0;

    told = SysTick->VAL;
    while (1) {
        tnow = SysTick->VAL;
        if (tnow != told) {
            if (tnow < told)
                tcnt += told - tnow;
            else
                tcnt += reload - tnow + told;
            told = tnow;
            if (tcnt >= ticks) {
                break;
            }
        }
    }
}

void SysTick_Handler(void)
{
    system_heartbeat_process();
}

void asm_cpu_reboot(void)
{
    __set_FAULTMASK(1);
    NVIC_SystemReset();
}

u64 asm_cpu_run_time_us(void)
{
    u64 us = cpu_run_ticks() * CONFIG_SYS_TICK_MS * 1000;
    return (us + (sys_tick_num_by_heartbeat - SysTick->VAL) / sys_tick_num_by_us);
}
