/* Copyright (C) 2026 NOS contributors */
#ifndef NOS_KERNEL_H
#define NOS_KERNEL_H
#include "arch.h"
#include "linux.h"
#ifdef NOS_MMU
#define USER_TEXT 0x00400000u
#define USER_DATA 0x01000000u
#ifndef USER_BYTES
#ifdef NOS_LINUX_COMPAT
#define USER_BYTES (256u * 1024u)
#else
#define USER_BYTES (64u * 1024u)
#endif
#endif
#ifndef HEAP_OFFSET
#ifdef NOS_LINUX_COMPAT
#define HEAP_OFFSET (128u * 1024u)
#else
#define HEAP_OFFSET (24u * 1024u)
#endif
#endif
#ifndef HEAP_END
#ifdef NOS_LINUX_COMPAT
#define HEAP_END (192u * 1024u)
#else
#define HEAP_END (48u * 1024u)
#endif
#endif
#ifndef PAGE_POOL_PAGES
#ifdef NOS_LINUX_COMPAT
#define PAGE_POOL_PAGES 8192u
#else
#define PAGE_POOL_PAGES 1024u
#endif
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
#ifndef VMA_SLOTS
#ifdef NOS_LINUX_COMPAT
#define VMA_SLOTS 128u
#else
#define VMA_SLOTS 16u
#endif
#endif
#ifndef VM_MAP_MAX
#ifdef NOS_LINUX_COMPAT
#define VM_MAP_MAX (16u * 1024u * 1024u)
#else
#define VM_MAP_MAX (512u * 1024u)
#endif
#endif
#ifndef NOS_USER_PT_SLOTS
#ifdef NOS_LINUX_COMPAT
#ifdef NOS_ARM
#define NOS_USER_PT_SLOTS 256u
#else
#define NOS_USER_PT_SLOTS 128u
#endif
#else
#define NOS_USER_PT_SLOTS 32u
#endif
#endif
#define VM_USER_BASE 0x00400000u
#ifdef NOS_LINUX_COMPAT
#define VM_USER_END 0x40000000u
#else
#define VM_USER_END 0x10000000u
#endif
_Static_assert(NOS_SMP >= 1 && NOS_SMP <= NOS_CPUS && NOS_IDLE_FIRST > 2, "CPU/task configuration");
_Static_assert(!(USER_BYTES % NOS_PAGE_SIZE), "task memory page alignment");
#ifdef NOS_MMU
_Static_assert(HEAP_END + NOS_PAGE_SIZE < USER_BYTES, "user stack and heap guard");
#endif
enum task_state { TASK_UNUSED, TASK_READY, TASK_SLEEP, TASK_RECV, TASK_REPLY, TASK_WAIT, TASK_LOCK, TASK_FUTEX, TASK_IRQ, TASK_SIGNAL_WAIT, TASK_EXEC, TASK_DEAD, TASK_RETIRED };
struct vma { uintptr_t va, pa; size_t size; unsigned prot, maxprot; };
struct task {
    struct arch_frame frame;
    enum task_state state;
    unsigned id, priority, base_priority, caps, generation, parent, mutex_caps;
    unsigned space_id, space_owner, run_cpu, pin_cpu, is_thread, endpoint_rights[NOS_ENDPOINTS];
    uintptr_t clear_tid, tls_memory, futex_address;
    unsigned futex_value, cancel_pending;
    unsigned futex_shared, futex_space;
    uintptr_t futex_key;
    uintptr_t memory, heap, heap_limit, brk;
    uintptr_t pending;
    unsigned wait_for, reply_from, endpoint_handle;
    uint64_t wake_tick;
    intptr_t exit_status;
    struct nos_message request;
    uint64_t request_order;
    unsigned request_sent;
    uintptr_t tls_slot, tls_value;
#ifdef NOS_MMU
    struct arch_frame linux_frame;
    struct arch_frame linux_syscall_frame;
    uintptr_t linux_syscall_number;
    unsigned linux_restart_pending;
    unsigned abi, linux_gateway_active;
    uintptr_t linux_gateway_entry, linux_gateway_stack, linux_robust_list;
    uint64_t signal_mask, signal_pending, signal_wait_set, signal_restore_mask;
    uintptr_t signal_wait_info, signal_stack_base, signal_stack_size;
    unsigned linux_stopped, signal_suspend, signal_depth;
    unsigned signal_wait_status, linux_exit_signal;
    unsigned linux_wait, linux_wait_options;
    unsigned linux_pgid, linux_sid, linux_execed;
    unsigned linux_waitcontrol;
    uint64_t linux_waitcontrol_mask;
    uintptr_t linux_sleep_remain;
    unsigned linux_sleep_wide;
    uint64_t linux_stack_soft, linux_stack_hard, linux_nofile_soft, linux_nofile_hard;
    uint64_t linux_alarm_deadline, linux_alarm_interval;
#endif
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
intptr_t vm_map_linux(struct task *task, uintptr_t address, size_t length, unsigned prot, unsigned flags);
int vm_image_map(struct task *task, uintptr_t va, size_t bytes, unsigned prot);
int vm_gateway_map(struct task *task, uintptr_t va, size_t bytes, unsigned prot);
intptr_t vm_gateway_unmap(struct task *task, uintptr_t va, size_t bytes);
intptr_t vm_linux_brk(struct task *task, uintptr_t address);
intptr_t vm_linux_unmap(struct task *task, uintptr_t address, size_t bytes);
intptr_t vm_linux_protect(struct task *task, uintptr_t address, unsigned prot, size_t bytes);
intptr_t vm_linux_sync(struct task *task, uintptr_t address, size_t bytes, unsigned flags);
int task_copy_load(struct task *task, uintptr_t dst, const void *src, size_t count);
uintptr_t task_user_phys(struct task *task, uintptr_t ptr);
uintptr_t task_user_phys_read(struct task *task, uintptr_t ptr);
int space_clone(struct task *child, struct task *parent, uintptr_t stack_top);
int space_fork(struct task *child, struct task *parent);
int space_commit_exec(struct task *current, struct task *replacement);
intptr_t vm_shared_create(struct task *broker, size_t bytes);
int vm_shared_grant(struct task *broker, unsigned handle, struct task *grantee, unsigned prot);
intptr_t vm_shared_map(struct task *task, unsigned handle, uintptr_t address, size_t bytes, size_t offset, unsigned prot, unsigned flags);
int vm_shared_release(struct task *broker, unsigned handle);
uintptr_t task_shared_phys(struct task *task, uintptr_t address);
void vm_shared_collect(void);
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
intptr_t task_exec_linux(uintptr_t request, uintptr_t entropy, uintptr_t flags);
intptr_t bootfile_operation(uintptr_t operation, uintptr_t request);
struct task *find_task(uintptr_t pid);
int create_task(unsigned slot, uintptr_t pc, uintptr_t arg, unsigned priority, unsigned caps, unsigned parent);
void task_reap(struct task *task);
int task_is_waitable(const struct task *task);
intptr_t task_spawn(uintptr_t pc, uintptr_t arg, uintptr_t priority);
intptr_t thread_create(uintptr_t args, uintptr_t priority);
intptr_t task_wait(uintptr_t pid, uintptr_t status, uintptr_t flags);
intptr_t futex_wait(uintptr_t address, uintptr_t expected, uintptr_t timeout);
intptr_t futex_wake(uintptr_t address, uintptr_t count);
intptr_t futex_requeue(uintptr_t request);
intptr_t futex_requeue_args(const struct nos_futex_requeue *args);
intptr_t futex_wait_mode(uintptr_t address, uintptr_t expected, uintptr_t timeout, unsigned shared);
intptr_t futex_wake_mode(uintptr_t address, uintptr_t count, unsigned shared);
intptr_t futex_requeue_mode(const struct nos_futex_requeue *args, unsigned shared);
unsigned futex_wake_task(struct task *task, uintptr_t address, unsigned count);
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
intptr_t ipc_xfer(uintptr_t request);
intptr_t endpoint_create(void);
intptr_t endpoint_close(uintptr_t handle);
intptr_t endpoint_grant(uintptr_t pid, uintptr_t handle, uintptr_t rights, int revoke);
intptr_t ipc_cancel(uintptr_t pid);
void ipc_timeouts(void);
int ipc_process_notice(intptr_t operation, unsigned process, unsigned value);
void ipc_interrupt_task(struct task *task, intptr_t error);
unsigned endpoint_count(void);
struct task *select_task(void);
void scheduler_tick(void);
int kernel_clock_snapshot(uintptr_t clock, struct nos_clock_value *value);
intptr_t irq_operation(unsigned nr, uintptr_t a, uintptr_t b);
void kernel_irq_notify(unsigned irq);
void irq_task_exit(struct task *task);
void irq_timeouts(void);
void kernel_secondary_start(void) __attribute__((noreturn));

#endif
