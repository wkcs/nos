/* Copyright (C) 2026 NOS contributors.
 * CPU context/interrupt machinery; SDK task APIs are implemented by NOS OSAL. */
#include "nos_port.h"
extern void vPortYield(void);
extern BaseType_t xPortStartScheduler(void);
static void task_entry(void *argument) {
    struct arch_frame *frame = argument;
    ((void (*)(uintptr_t, uintptr_t))frame->entry)(frame->argument, frame->second);
    nos_idf_syscall(NOS_SYS_EXIT, 0, 0, 0);
    for (;;)
        vPortYield();
}
void arch_frame_init(struct arch_frame *f, uintptr_t pc, uintptr_t sp, uintptr_t arg,
                     uintptr_t second) {
    k_memset(f, 0, sizeof(*f));
    f->entry = f->pc = pc;
    f->argument = arg;
    f->second = second;
    f->sp = sp;
    sp &= ~(uintptr_t)15;
    f->stack_end = sp;
    f->context = (uintptr_t)pxPortInitialiseStack((StackType_t *)sp, task_entry, f);
}
struct arch_frame *arch_return_frame(struct arch_frame *f) {
    return f;
}
void arch_enter_user(struct arch_frame *f) {
    (void)f;
    nos_idf_scheduler_ready();
    xPortStartScheduler();
    kernel_fail("NOS CPU context start returned");
}
uintptr_t arch_irq_save(void) {
    return portSET_INTERRUPT_MASK_FROM_ISR();
}
void arch_irq_restore(uintptr_t value) {
    portCLEAR_INTERRUPT_MASK_FROM_ISR(value);
}
void arch_cache_sync(void) {
    __asm__ volatile("memw" ::: "memory");
}
void nos_cpu_halt(void) {
    for (;;)
        __asm__ volatile("waiti 15");
}
