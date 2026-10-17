/* Copyright (C) 2026 NOS contributors */
#ifndef NOS_LINUX_H
#define NOS_LINUX_H
#include "abi.h"
#define NOS_ABI_NATIVE 0u
#define NOS_ABI_LINUX 1u
#define NOS_ABI_GATEWAY 2u
#define NOS_LINUX_GATEWAY_BASE 0x01800000u
#define NOS_LINUX_GATEWAY_END 0x01900000u
#define NOS_LINUX_GATEWAY_STACK_BASE 0x01900000u
#define NOS_LINUX_GATEWAY_STACK_STRIDE 0x4000u
#define NOS_LINUX_GATEWAY_STACK_BYTES 0x2000u
#define NOS_LINUX_GATEWAY_RESERVED_END 0x01a00000u
#define NOS_EXEC_RANDOM_VALID 1u
#define NOS_LINUX_WAIT_BEGIN 0u
#define NOS_LINUX_WAIT_SLEEP 1u
#define NOS_LINUX_WAIT_END 2u
/* WAITCONTROL: BEGIN(b=optional uint64_t mask,c=8), SLEEP(b=ticks), END.
 * BEGIN..END is confined to one gateway call. RETURN also ends it. */
struct nos_linux_call { uintptr_t number, args[6]; };
struct nos_linux_map { uintptr_t address, length, prot, flags, fd, offset; };
/* Native EXEC_LINUX(a=&nos_exec_request,b=random[16],c=RANDOM_VALID).
 * Entropy is supplied by the supervising caller, normally VFS virtio-rng. */
void nos_linux_gateway(struct nos_linux_call *call) __attribute__((noreturn));
struct task;
intptr_t linux_dispatch(uintptr_t number, const uintptr_t arguments[6]);
intptr_t linux_gateway_return(uintptr_t result);
int linux_task_prepare(struct task *task, uintptr_t gateway_entry);
int linux_gateway_clone(struct task *child, const struct task *parent);
void linux_task_exit(struct task *task);
intptr_t linux_exec_current(uintptr_t request, uintptr_t entropy, uintptr_t flags);
void linux_exec_progress(void);
void linux_timer_tick(void);
unsigned linux_process_group(const struct task *task);
unsigned linux_process_session(const struct task *task);
int linux_wait_wake(struct task *waiter, struct task *child);
intptr_t linux_wait_control(uintptr_t operation, uintptr_t argument, uintptr_t size);
void linux_wait_control_end(struct task *task);
#define NOS_LINUX_NOT_HANDLED INTPTR_MIN
intptr_t linux_signal_syscall(uintptr_t number, const uintptr_t arguments[6]);
void linux_signal_deliver(struct task *task);
void linux_signal_tick(void);
void linux_signal_reset(struct task *task);
void linux_signal_fork(struct task *child, const struct task *parent);
void linux_signal_exec(struct task *task, unsigned old_space);
void linux_signal_retid(struct task *source, struct task *destination);
void linux_signal_interrupted(struct task *task);
void linux_signal_discard(struct task *task);
int linux_signal_send(struct task *task, unsigned signal, int code, unsigned sender, uintptr_t value);
#endif
