/* Copyright (C) 2026 NOS contributors */
#ifndef NOS_ARCH_H
#define NOS_ARCH_H
#include "abi.h"
#define ARCH_MAP_PRIVILEGED 0x100u
#if defined(NOS_ARM64)
struct arch_frame { uintptr_t x[31], sp, pc, status, tls, padding; uint64_t q[64]; uintptr_t fpcr, fpsr; };
#define ARCH_NAME "arm64"
#define FRAME_RESULT(f) ((f)->x[0])
#elif defined(NOS_RISCV32)
struct arch_frame { uintptr_t x[32], pc, status; uint64_t f[32]; uintptr_t fcsr; };
#define ARCH_NAME "riscv32"
#define FRAME_RESULT(f) ((f)->x[10])
#elif defined(NOS_RISCV)
struct arch_frame { uintptr_t x[32], pc, status; uint64_t f[32]; uintptr_t fcsr; };
#define ARCH_NAME "riscv64"
#define FRAME_RESULT(f) ((f)->x[10])
#elif defined(NOS_ARM_M)
struct arch_frame { uintptr_t saved[8], r[4], ip, lr, pc, status, sp; };
#ifdef NOS_NK60_V2
#define ARCH_NAME "stm32f401-nk60_v2"
#else
#define ARCH_NAME "stm32f405"
#endif
#define FRAME_RESULT(f) ((f)->r[0])
#else
struct arch_frame { uintptr_t r[13], sp, lr, pc, status, tls; uint64_t d[32]; uintptr_t fpscr, padding; };
#define ARCH_NAME "armv7"
#define FRAME_RESULT(f) ((f)->r[0])
#endif
enum arch_event { ARCH_SYSCALL, ARCH_TIMER, ARCH_FAULT, ARCH_IRQ };
#if defined(NOS_ARM64)
_Static_assert(offsetof(struct arch_frame, sp) == 248 && offsetof(struct arch_frame, tls) == 272 &&
               offsetof(struct arch_frame, q) == 288 && sizeof(struct arch_frame) == 816, "AArch64 trap ABI");
#elif defined(NOS_RISCV32)
_Static_assert(offsetof(struct arch_frame, pc) == 128 && offsetof(struct arch_frame, f) == 136 &&
               offsetof(struct arch_frame, fcsr) == 392 && sizeof(struct arch_frame) == 400, "RV32 trap ABI");
#elif defined(NOS_RISCV)
_Static_assert(offsetof(struct arch_frame, pc) == 256 && offsetof(struct arch_frame, f) == 272 &&
               offsetof(struct arch_frame, fcsr) == 528, "RISC-V trap ABI");
#elif defined(NOS_ARM_M)
_Static_assert(offsetof(struct arch_frame, sp) == 64, "Cortex-M exception ABI");
#else
_Static_assert(offsetof(struct arch_frame, pc) == 60 && offsetof(struct arch_frame, tls) == 68 && offsetof(struct arch_frame, d) == 72 &&
               offsetof(struct arch_frame, fpscr) == 328 && sizeof(struct arch_frame) == 336, "ARM exception ABI");
#endif
struct task;
unsigned arch_cpu_id(void);
unsigned arch_cpu_count(void);
void arch_start_secondary(void);
void arch_secondary_init(void);
void arch_tlb_sync(void);
void arch_cache_sync(void);
void arch_device_revoke(struct task *task);
intptr_t arch_device_operation(struct task *task, uintptr_t op, uintptr_t b, uintptr_t c);
#ifdef NOS_MPU
int arch_mpu_valid_range(uintptr_t base, size_t size);
#endif
void arch_platform_init(void);
uint64_t arch_realtime_ns(void);
uintptr_t arch_fault_address(void);
unsigned arch_fault_signal(void);
#if defined(NOS_MMU) && defined(NOS_LINUX_COMPAT)
unsigned arch_fault_access(void);
#endif
#if defined(NOS_ARM) || defined(NOS_ARM_M)
uintptr_t arch_user_helper_phys(uintptr_t va);
#else
static inline uintptr_t arch_user_helper_phys(uintptr_t va) { (void)va; return 0; }
#endif
void arch_putc(char c);
void arch_shutdown(int failed) __attribute__((noreturn));
void arch_timer_init(void);
void arch_timer_ack(void);
unsigned arch_irq_claim(void);
void arch_irq_complete(unsigned irq);
int arch_irq_bind(unsigned device_index);
void arch_irq_mask(unsigned irq, int masked);
uintptr_t arch_irq_save(void);
void arch_irq_restore(uintptr_t value);
void arch_space_init(unsigned id);
void arch_space_switch(unsigned id);
int arch_map(unsigned id, uintptr_t va, uintptr_t pa, unsigned flags);
void arch_unmap(unsigned id, uintptr_t va);
unsigned arch_mapping_flags(unsigned id, uintptr_t va);
void arch_fault_probe(void);
void arch_frame_init(struct arch_frame *f, uintptr_t pc, uintptr_t sp, uintptr_t arg, uintptr_t second);
struct arch_frame *arch_return_frame(struct arch_frame *f);
void arch_enter_user(struct arch_frame *frame) __attribute__((noreturn));
struct arch_frame *kernel_trap(struct arch_frame *frame, unsigned event);
void kernel_start(void) __attribute__((noreturn));
#endif
