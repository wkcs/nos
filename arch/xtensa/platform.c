/* Copyright (C) 2026 NOS contributors. ESP-IDF Xtensa hardware mechanisms. */
#include "nos_port.h"
#include "esp_rom_sys.h"
#include "esp_rom_uart.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "xtensa/hal.h"
#include "xtensa_context.h"
extern void *pxCurrentTCBs[];
extern void vPortYield(void);
extern BaseType_t xPortStartScheduler(void);
static void task_entry(void *argument)
{
    struct arch_frame *frame = argument;
    ((void (*)(uintptr_t, uintptr_t))frame->entry)(frame->argument, frame->second);
    nos_xtensa_syscall(NOS_SYS_EXIT, 0, 0, 0);
    for (;;)
        vPortYield();
}
void arch_frame_init(struct arch_frame *f, uintptr_t pc, uintptr_t sp, uintptr_t arg,
                     uintptr_t second)
{
    k_memset(f, 0, sizeof(*f));
    f->entry = f->pc = pc;
    f->argument = arg;
    f->second = second;
    f->sp = sp;
    sp &= ~(uintptr_t)15;
    f->stack_end = sp;
    f->context = (uintptr_t)pxPortInitialiseStack((StackType_t *)sp, task_entry, f);
}
struct arch_frame *arch_return_frame(struct arch_frame *f)
{
    return f;
}
void arch_enter_user(struct arch_frame *f)
{
    (void)f;
    nos_idf_scheduler_ready();
    xPortStartScheduler();
    kernel_fail("Xtensa scheduler returned");
}
void arch_platform_init(void) {}
void arch_putc(char c)
{
    esp_rom_uart_tx_one_char((uint8_t)c);
}
void arch_shutdown(int failed)
{
    esp_rom_printf("NOS HALT %d\n", failed);
    for (;;)
        __asm__ volatile("waiti 15");
}
uintptr_t arch_irq_save(void)
{
    return portSET_INTERRUPT_MASK_FROM_ISR();
}
void arch_irq_restore(uintptr_t value)
{
    portCLEAR_INTERRUPT_MASK_FROM_ISR(value);
}
unsigned arch_cpu_id(void)
{
    return 0;
}
unsigned arch_cpu_count(void)
{
    return 1;
}
void arch_start_secondary(void) {}
void arch_secondary_init(void) {}
void arch_timer_init(void) {}
void arch_timer_ack(void) {}
uint64_t arch_realtime_ns(void)
{
    return 0;
}
uintptr_t arch_fault_address(void)
{
    return 0;
}
unsigned arch_fault_signal(void)
{
    return 11;
}
void arch_fault_probe(void)
{
    kernel_fail("Xtensa fault probe unsupported");
}
void arch_space_init(unsigned id)
{
    (void)id;
}
void arch_space_switch(unsigned id)
{
    (void)id;
}
int arch_map(unsigned id, uintptr_t va, uintptr_t pa, unsigned flags)
{
    (void)id;
    (void)flags;
    return va == pa ? 0 : -NOS_ENOTSUP;
}
void arch_unmap(unsigned id, uintptr_t va)
{
    (void)id;
    (void)va;
}
unsigned arch_mapping_flags(unsigned id, uintptr_t va)
{
    (void)id;
    (void)va;
    return 0;
}
void arch_tlb_sync(void) {}
void arch_cache_sync(void)
{
    __asm__ volatile("memw" ::: "memory");
}
unsigned arch_irq_claim(void)
{
    return 0;
}
void arch_irq_complete(unsigned irq)
{
    (void)irq;
}
int arch_irq_bind(unsigned device)
{
    (void)device;
    return -NOS_ENOTSUP;
}
void arch_irq_mask(unsigned irq, int masked)
{
    (void)irq;
    (void)masked;
}
void arch_device_revoke(struct task *task)
{
    (void)task;
}
intptr_t arch_device_operation(struct task *task, uintptr_t op, uintptr_t a, uintptr_t b)
{
    (void)task;
    (void)op;
    (void)a;
    (void)b;
    return -NOS_ENOTSUP;
}
int arch_shared_user_pointer(uintptr_t address, int write)
{
    const void *p = (const void *)address;
    return esp_ptr_byte_accessible(p) || esp_ptr_in_rtc_slow(p) || (!write && esp_ptr_in_drom(p));
}

int arch_syscall_ready(void)
{
    return xTaskGetSchedulerState() == taskSCHEDULER_RUNNING && !xPortInIsrContext();
}
