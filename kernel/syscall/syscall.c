/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS Microkernel 16-Entry System Call Dispatcher
 */

#define pr_fmt(fmt) "[SYSCALL]:%s[%d]:"fmt, __func__, __LINE__

#include <nos/syscall.h>
#include <nos/cap.h>
#include <nos/ipc.h>
#include <nos/hal_arch.h>
#include <uapi/nos/abi.h>
#include <uapi/nos/errno.h>
#include <kernel/kernel.h>
#include <kernel/task.h>
#include <kernel/sched.h>
#include <kernel/cpu.h>
#include <kernel/console.h>

/* Thread control operations */
#define NOS_THREAD_OP_SET_PRIO  1U
#define NOS_THREAD_OP_SUSPEND   2U
#define NOS_THREAD_OP_RESUME    3U
#define NOS_THREAD_OP_KILL      4U

/* Capability object creation sub-ops for SYS_NOS_CAP_CONTROL */
#define NOS_CAP_OP_CREATE_EP    10U
#define NOS_CAP_OP_CREATE_NOTIF 11U

static long sys_nos_yield(uintptr_t a0, uintptr_t a1, uintptr_t a2,
                          uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    return (long)task_yield_cpu();
}

static long sys_nos_exit(uintptr_t a0, uintptr_t a1, uintptr_t a2,
                         uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    task_del(current);
    return 0;
}

static long sys_nos_debug_console(uintptr_t buf_ptr, uintptr_t len, uintptr_t is_read,
                                  uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
    const char *buf = (const char *)buf_ptr;

    (void)a3;
    (void)a4;
    (void)a5;

    if (buf == NULL && len > 0U) {
        return -EINVAL;
    }
    if (len == 0U) {
        return 0;
    }
    if (is_read) {
        return -ENOSYS;
    }
    console_write(buf, (size_t)len);
    return (long)len;
}

static long sys_nos_clock_get(uintptr_t out_ns_ptr, uintptr_t a1, uintptr_t a2,
                              uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
    uint64_t *out_ns = (uint64_t *)out_ns_ptr;
    uint64_t ns = (uint64_t)cpu_run_time_us() * 1000ULL;

    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;

    if (out_ns != NULL) {
        *out_ns = ns;
        return 0;
    }
    return (long)ns;
}

static long sys_nos_sleep_until(uintptr_t ticks, uintptr_t a1, uintptr_t a2,
                                uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;

    if (ticks == 0U) {
        return (long)task_yield_cpu();
    }
    return (long)task_sleep((u32)ticks);
}

static long sys_nos_thread_create(uintptr_t name_ptr,
                                  uintptr_t entry_ptr,
                                  uintptr_t param_ptr,
                                  uintptr_t prio,
                                  uintptr_t stack_size,
                                  uintptr_t out_cap_ptr)
{
    const char *name = (name_ptr != 0U) ? (const char *)name_ptr : "uthread";
    struct task_struct *child;
    nos_handle_t cap = NOS_HANDLE_INVALID;
    int rc;

    if (entry_ptr == 0U) {
        return -EINVAL;
    }
    if (stack_size == 0U) {
        stack_size = 1024U;
    }

    child = task_create(name,
                        (void (*)(void *))entry_ptr,
                        (void *)param_ptr,
                        (uint8_t)prio,
                        (uint32_t)stack_size,
                        10U,
                        NULL);
    if (child == NULL) {
        return -ENOMEM;
    }

    if (current != NULL) {
        rc = nos_cap_alloc(&current->cap_table,
                           NOS_CAP_TASK,
                           child,
                           NOS_CAP_RIGHT_ALL,
                           (uint32_t)child->pid,
                           &cap);
        if (rc < 0) {
            task_del(child);
            return rc;
        }
    }

    if (out_cap_ptr != 0U) {
        *(nos_handle_t *)out_cap_ptr = cap;
    }
    (void)task_ready(child);
    return (long)cap;
}

static long sys_nos_thread_control(uintptr_t task_cap,
                                   uintptr_t op,
                                   uintptr_t arg0,
                                   uintptr_t a3,
                                   uintptr_t a4,
                                   uintptr_t a5)
{
    struct task_struct *target = NULL;
    int rc;

    (void)a3;
    (void)a4;
    (void)a5;

    if (current == NULL) {
        return -EINVAL;
    }

    rc = nos_cap_lookup(&current->cap_table,
                        (nos_handle_t)task_cap,
                        NOS_CAP_TASK,
                        NOS_CAP_RIGHT_MANAGE,
                        (void **)&target,
                        NULL);
    if (rc < 0) {
        return rc;
    }

    switch (op) {
    case NOS_THREAD_OP_SET_PRIO:
        return (long)task_set_prio(target, (uint8_t)arg0);
    case NOS_THREAD_OP_SUSPEND:
        return (long)task_hang(target);
    case NOS_THREAD_OP_RESUME:
        return (long)task_resume(target);
    case NOS_THREAD_OP_KILL:
        task_del(target);
        return 0;
    default:
        return -EINVAL;
    }
}

static long sys_nos_vm_map(uintptr_t vm_cap,
                           uintptr_t vaddr,
                           uintptr_t paddr,
                           uintptr_t size,
                           uintptr_t prot_flags,
                           uintptr_t a5)
{
    struct nos_vm_space *space = NULL;
    int rc;

    (void)a5;

    if (current == NULL) {
        return -EINVAL;
    }
    if (vm_cap != NOS_HANDLE_INVALID) {
        rc = nos_cap_lookup(&current->cap_table,
                            (nos_handle_t)vm_cap,
                            NOS_CAP_VM_SPACE,
                            NOS_CAP_RIGHT_WRITE,
                            (void **)&space,
                            NULL);
        if (rc < 0) {
            return rc;
        }
    }
    return (long)hal_vm_map(space, vaddr, paddr, (size_t)size, (uint32_t)prot_flags);
}

static long sys_nos_vm_unmap(uintptr_t vm_cap,
                             uintptr_t vaddr,
                             uintptr_t size,
                             uintptr_t a3,
                             uintptr_t a4,
                             uintptr_t a5)
{
    struct nos_vm_space *space = NULL;
    int rc;

    (void)a3;
    (void)a4;
    (void)a5;

    if (current == NULL) {
        return -EINVAL;
    }
    if (vm_cap != NOS_HANDLE_INVALID) {
        rc = nos_cap_lookup(&current->cap_table,
                            (nos_handle_t)vm_cap,
                            NOS_CAP_VM_SPACE,
                            NOS_CAP_RIGHT_WRITE,
                            (void **)&space,
                            NULL);
        if (rc < 0) {
            return rc;
        }
    }
    return (long)hal_vm_unmap(space, vaddr, (size_t)size);
}

static long sys_nos_ipc_call(uintptr_t ep_cap,
                             uintptr_t req_ptr,
                             uintptr_t resp_ptr,
                             uintptr_t timeout_ticks,
                             uintptr_t a4,
                             uintptr_t a5)
{
    (void)a4;
    (void)a5;
    return (long)nos_ipc_call((nos_handle_t)ep_cap,
                              (const struct nos_ipc_msg *)req_ptr,
                              (struct nos_ipc_msg *)resp_ptr,
                              (uint32_t)timeout_ticks);
}

static long sys_nos_ipc_recv(uintptr_t ep_cap,
                             uintptr_t msg_ptr,
                             uintptr_t timeout_ticks,
                             uintptr_t a3,
                             uintptr_t a4,
                             uintptr_t a5)
{
    (void)a3;
    (void)a4;
    (void)a5;
    return (long)nos_ipc_recv((nos_handle_t)ep_cap,
                              (struct nos_ipc_msg *)msg_ptr,
                              (uint32_t)timeout_ticks);
}

static long sys_nos_ipc_reply_wait(uintptr_t ep_cap,
                                   uintptr_t reply_ptr,
                                   uintptr_t next_req_ptr,
                                   uintptr_t timeout_ticks,
                                   uintptr_t a4,
                                   uintptr_t a5)
{
    (void)a4;
    (void)a5;
    return (long)nos_ipc_reply_wait((nos_handle_t)ep_cap,
                                    (const struct nos_ipc_msg *)reply_ptr,
                                    (struct nos_ipc_msg *)next_req_ptr,
                                    (uint32_t)timeout_ticks);
}

static long sys_nos_notify_signal(uintptr_t notif_cap,
                                  uintptr_t bits_lo,
                                  uintptr_t bits_hi,
                                  uintptr_t a3,
                                  uintptr_t a4,
                                  uintptr_t a5)
{
    uint64_t bits = ((uint64_t)bits_hi << 32) | (uint32_t)bits_lo;
    (void)a3;
    (void)a4;
    (void)a5;
    return (long)nos_notify_signal((nos_handle_t)notif_cap, bits);
}

static long sys_nos_notify_wait(uintptr_t notif_cap,
                                uintptr_t out_bits_ptr,
                                uintptr_t timeout_ticks,
                                uintptr_t a3,
                                uintptr_t a4,
                                uintptr_t a5)
{
    (void)a3;
    (void)a4;
    (void)a5;
    return (long)nos_notify_wait((nos_handle_t)notif_cap,
                                 (uint64_t *)out_bits_ptr,
                                 (uint32_t)timeout_ticks);
}

static long sys_nos_cap_control(uintptr_t op,
                                uintptr_t handle,
                                uintptr_t arg0,
                                uintptr_t arg1,
                                uintptr_t out_ptr,
                                uintptr_t a5)
{
    nos_handle_t out_h = NOS_HANDLE_INVALID;
    int rc;

    (void)a5;

    if (current == NULL) {
        return -EINVAL;
    }

    switch (op) {
    case NOS_CAP_OP_DERIVE:
        rc = nos_cap_derive(&current->cap_table,
                            (nos_handle_t)handle,
                            &current->cap_table,
                            (nos_cap_rights_t)arg0,
                            (uint32_t)arg1,
                            &out_h);
        if (rc == 0 && out_ptr != 0U) {
            *(nos_handle_t *)out_ptr = out_h;
        }
        return (rc < 0) ? (long)rc : (long)out_h;

    case NOS_CAP_OP_REVOKE:
        return (long)nos_cap_revoke(&current->cap_table, (nos_handle_t)handle);

    case NOS_CAP_OP_INSPECT:
        return (long)nos_cap_inspect(&current->cap_table,
                                     (nos_handle_t)handle,
                                     (uint32_t *)arg0,
                                     (nos_cap_rights_t *)arg1);

    case NOS_CAP_OP_CREATE_EP:
        if (nos_endpoint_create(&out_h) == NULL) {
            return -ENOMEM;
        }
        if (out_ptr != 0U) {
            *(nos_handle_t *)out_ptr = out_h;
        }
        return (long)out_h;

    case NOS_CAP_OP_CREATE_NOTIF:
        if (nos_notification_create(&out_h) == NULL) {
            return -ENOMEM;
        }
        if (out_ptr != 0U) {
            *(nos_handle_t *)out_ptr = out_h;
        }
        return (long)out_h;

    default:
        return -EINVAL;
    }
}

static long sys_nos_irq_control(uintptr_t op,
                                uintptr_t irq_num,
                                uintptr_t notif_cap,
                                uintptr_t a3,
                                uintptr_t a4,
                                uintptr_t a5)
{
    (void)notif_cap;
    (void)a3;
    (void)a4;
    (void)a5;

    switch (op) {
    case NOS_IRQ_OP_BIND:
        hal_irq_enable((uint32_t)irq_num);
        return 0;
    case NOS_IRQ_OP_UNBIND:
        hal_irq_disable((uint32_t)irq_num);
        return 0;
    case NOS_IRQ_OP_ACK:
        hal_irq_ack((uint32_t)irq_num);
        hal_irq_enable((uint32_t)irq_num);
        return 0;
    default:
        return -EINVAL;
    }
}

static const nos_syscall_fn_t g_syscall_table[SYS_NOS_MAX] = {
    [SYS_NOS_YIELD]          = sys_nos_yield,
    [SYS_NOS_EXIT]           = sys_nos_exit,
    [SYS_NOS_DEBUG_CONSOLE]  = sys_nos_debug_console,
    [SYS_NOS_CLOCK_GET]      = sys_nos_clock_get,
    [SYS_NOS_SLEEP_UNTIL]    = sys_nos_sleep_until,
    [SYS_NOS_THREAD_CREATE]  = sys_nos_thread_create,
    [SYS_NOS_THREAD_CONTROL] = sys_nos_thread_control,
    [SYS_NOS_VM_MAP]         = sys_nos_vm_map,
    [SYS_NOS_VM_UNMAP]       = sys_nos_vm_unmap,
    [SYS_NOS_IPC_CALL]       = sys_nos_ipc_call,
    [SYS_NOS_IPC_RECV]       = sys_nos_ipc_recv,
    [SYS_NOS_IPC_REPLY_WAIT] = sys_nos_ipc_reply_wait,
    [SYS_NOS_NOTIFY_SIGNAL]  = sys_nos_notify_signal,
    [SYS_NOS_NOTIFY_WAIT]    = sys_nos_notify_wait,
    [SYS_NOS_CAP_CONTROL]    = sys_nos_cap_control,
    [SYS_NOS_IRQ_CONTROL]    = sys_nos_irq_control,
};

long nos_syscall_dispatch(unsigned long sysno,
                          uintptr_t a0,
                          uintptr_t a1,
                          uintptr_t a2,
                          uintptr_t a3,
                          uintptr_t a4,
                          uintptr_t a5)
{
    if (sysno >= SYS_NOS_MAX || g_syscall_table[sysno] == NULL) {
        return -ENOSYS;
    }
    return g_syscall_table[sysno](a0, a1, a2, a3, a4, a5);
}
