/* Copyright (C) 2026 NOS contributors */
#ifndef NOS_KERNEL_H
#define NOS_KERNEL_H
#include "arch.h"
#ifdef NOS_MMU
#define USER_TEXT 0x00400000u
#define USER_DATA 0x01000000u
#ifndef USER_BYTES
#define USER_BYTES (64u * 1024u)
#endif
#ifndef HEAP_OFFSET
#define HEAP_OFFSET (24u * 1024u)
#endif
#ifndef HEAP_END
#define HEAP_END (48u * 1024u)
#endif
#ifndef PAGE_POOL_PAGES
#define PAGE_POOL_PAGES 1024u
#endif
#else
#ifndef USER_BYTES
#define USER_BYTES (8u * 1024u)
#endif
#ifndef HEAP_OFFSET
#define HEAP_OFFSET 0u
#endif
#ifndef HEAP_END
#define HEAP_END (4u * 1024u)
#endif
#ifndef PAGE_POOL_PAGES
#ifdef NOS_RISCV32
#define PAGE_POOL_PAGES 256u
#else
#define PAGE_POOL_PAGES 20u
#endif
#endif
#endif
#define VMA_SLOTS 16u
_Static_assert(NOS_SMP >= 1 && NOS_SMP <= NOS_CPUS && NOS_IDLE_FIRST > 2, "CPU/task configuration");
_Static_assert(!(USER_BYTES % NOS_PAGE_SIZE), "task memory page alignment");
#ifdef NOS_MMU
_Static_assert(HEAP_END + NOS_PAGE_SIZE < USER_BYTES, "user stack and heap guard");
#endif
enum task_state { TASK_UNUSED, TASK_READY, TASK_SLEEP, TASK_RECV, TASK_REPLY, TASK_WAIT, TASK_LOCK, TASK_FUTEX, TASK_IRQ, TASK_DEAD, TASK_RETIRED };
struct vma { uintptr_t va, pa; size_t size; unsigned prot; };
struct task {
    struct arch_frame frame;
    enum task_state state;
    unsigned id, priority, base_priority, caps, generation, parent, mutex_caps;
    unsigned space_id, space_owner, run_cpu, pin_cpu, is_thread, endpoint_rights[NOS_ENDPOINTS];
    uintptr_t clear_tid, tls_memory, futex_address;
    unsigned futex_value, cancel_pending;
    uintptr_t memory, heap, heap_limit, brk;
    struct vma vma[VMA_SLOTS];
    uintptr_t pending;
    unsigned wait_for, reply_from, endpoint_handle;
    uint64_t wake_tick;
    intptr_t exit_status;
    struct nos_message request;
    uint64_t request_order;
    unsigned request_sent;
    uintptr_t tls_slot, tls_value;
};
struct spinlock { unsigned value; };
extern struct task tasks[NOS_TASKS];
extern struct task *cpu_tasks[NOS_CPUS];
#define current_task (cpu_tasks[arch_cpu_id()])
extern unsigned reschedule;
extern uint64_t kernel_ticks;
extern unsigned timer_switches;
extern char __user_text_start[], __user_text_end[], __user_text_load[];
extern char __user_data_start[], __user_data_end[], __user_data_load[];
extern char __user_bss_start[], __user_bss_end[];
void *k_memcpy(void *dst, const void *src, size_t count);
void *k_memset(void *dst, int value, size_t count);
void *k_memmove(void *dst, const void *src, size_t count);
int k_memcmp(const void *a, const void *b, size_t count);
void kernel_print(const char *text);
void kernel_fail(const char *text) __attribute__((noreturn));
uintptr_t spin_lock_irqsave(struct spinlock *lock);
void spin_unlock_irqrestore(struct spinlock *lock, uintptr_t saved);
void page_init(void);
#ifdef NOS_SELFTEST
int memory_transaction_selftest(void);
#endif
uintptr_t page_alloc(unsigned count);
int page_free(uintptr_t pa, unsigned count);
unsigned page_available(void);
int space_create(struct task *task);
int space_create_empty(struct task *task);
intptr_t vm_map_at(struct task *task, uintptr_t va, unsigned prot, size_t bytes);
int task_copy_load(struct task *task, uintptr_t dst, const void *src, size_t count);
uintptr_t task_user_phys(struct task *task, uintptr_t ptr);
uintptr_t task_user_phys_read(struct task *task, uintptr_t ptr);
int space_clone(struct task *child, struct task *parent, uintptr_t stack_top);
int task_copy_in(struct task *task, void *dst, uintptr_t src, size_t count);
void space_destroy(struct task *task);
int user_access(struct task *task, uintptr_t ptr, size_t size, int write);
int user_exec_access(struct task *task, uintptr_t pc);
int user_copy_in(void *dst, uintptr_t src, size_t count);
int user_copy_out(uintptr_t dst, const void *src, size_t count);
int task_copy_out(struct task *task, uintptr_t dst, const void *src, size_t count);
intptr_t vm_map(struct task *task, unsigned prot, size_t bytes);
intptr_t vm_unmap(struct task *task, uintptr_t va, size_t bytes);
intptr_t vm_protect(struct task *task, uintptr_t va, unsigned prot, size_t bytes);
intptr_t vm_load_protect(struct task *task, uintptr_t va, unsigned prot, size_t bytes);
intptr_t vm_brk(struct task *task, uintptr_t address);
void task_finish(struct task *task, intptr_t status);
struct task *find_task(uintptr_t pid);
int create_task(unsigned slot, uintptr_t pc, uintptr_t arg, unsigned priority, unsigned caps, unsigned parent);
void task_reap(struct task *task);
intptr_t task_spawn(uintptr_t pc, uintptr_t arg, uintptr_t priority);
intptr_t thread_create(uintptr_t args, uintptr_t priority);
intptr_t task_wait(uintptr_t pid, uintptr_t status);
intptr_t futex_wait(uintptr_t address, uintptr_t expected, uintptr_t timeout);
intptr_t futex_wake(uintptr_t address, uintptr_t count);
intptr_t futex_requeue(uintptr_t request);
void futex_task_exit(struct task *task);
void mutex_recompute(void);
struct task *task_dependency(struct task *task);
void mutex_task_exit(struct task *task);
intptr_t mutex_operation(unsigned number, uintptr_t handle);
unsigned mutex_count(void);
intptr_t mutex_grant(uintptr_t pid, uintptr_t handle, uintptr_t rights, int revoke);
void ipc_init(void);
void ipc_task_exit(struct task *task);
void ipc_task_inherit(struct task *task, const struct task *parent);
intptr_t ipc_call(uintptr_t endpoint, uintptr_t request, uintptr_t reply, uint64_t timeout);
intptr_t ipc_recv(uintptr_t endpoint, uintptr_t buffer);
intptr_t ipc_reply(uintptr_t pid, uintptr_t buffer);
intptr_t endpoint_create(void);
intptr_t endpoint_close(uintptr_t handle);
intptr_t endpoint_grant(uintptr_t pid, uintptr_t handle, uintptr_t rights, int revoke);
intptr_t ipc_cancel(uintptr_t pid);
void ipc_timeouts(void);
unsigned endpoint_count(void);
struct task *select_task(void);
void scheduler_tick(void);
intptr_t irq_operation(unsigned nr, uintptr_t a, uintptr_t b);
void kernel_irq_notify(unsigned irq);
void irq_task_exit(struct task *task);
void irq_timeouts(void);
void kernel_secondary_start(void) __attribute__((noreturn));

#endif
