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
#include <nos/uaccess.h>
#include <uapi/nos/abi.h>
#include <uapi/nos/errno.h>
#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <kernel/task.h>
#include <kernel/sched.h>
#include <kernel/cpu.h>
#include <kernel/console.h>
#include <string.h>
#ifdef CONFIG_NOS_AI_CORE
#include <nos/accel.h>
#include <nos/vmo.h>
#include <nos/cell.h>
#include <nos/ring.h>
#include <nos/sched_infer.h>
#include <uapi/nos/ai.h>
#endif

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
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    task_exit_with_code((int)a0);
    return 0;
}

static long sys_nos_debug_console(uintptr_t buf_ptr, uintptr_t len, uintptr_t is_read,
                                  uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
    char kbuf[128];
    size_t done = 0U;

    (void)a3;
    (void)a4;
    (void)a5;

    if (len == 0U) {
        return 0;
    }
    if (buf_ptr == 0U) {
        return -EINVAL;
    }
    if (is_read) {
        return -ENOSYS;
    }

    while (done < (size_t)len) {
        size_t chunk = ((size_t)len - done > sizeof(kbuf)) ? sizeof(kbuf) : ((size_t)len - done);
        int rc = nos_copy_from_user(kbuf, buf_ptr + done, chunk);
        if (rc < 0) {
            return (done > 0U) ? (long)done : (long)rc;
        }
        console_write(kbuf, (int)chunk);
        done += chunk;
    }
    return (long)done;
}

static long sys_nos_clock_get(uintptr_t out_ns_ptr, uintptr_t a1, uintptr_t a2,
                              uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
    uint64_t ns = (uint64_t)cpu_run_time_us() * 1000ULL;

    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;

    if (out_ns_ptr != 0U) {
        int rc = nos_copy_to_user(out_ns_ptr, &ns, sizeof(ns));
        return (rc < 0) ? (long)rc : 0L;
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
    static char g_uthread_names[16][24];
    static uint32_t g_uthread_name_seq;
    const char *name = "uthread";
    struct task_struct *child;
    nos_handle_t cap = NOS_HANDLE_INVALID;
    int rc;

    if (entry_ptr == 0U) {
        return -EINVAL;
    }
    if (name_ptr != 0U) {
        uint32_t slot = __atomic_fetch_add(&g_uthread_name_seq, 1U, __ATOMIC_RELAXED) & 15U;
        if (nos_strncpy_from_user(g_uthread_names[slot], name_ptr, sizeof(g_uthread_names[slot])) > 0) {
            name = g_uthread_names[slot];
        } else {
            return -EFAULT;
        }
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
        rc = nos_cap_alloc(nos_task_active_cap_table(current),
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
        rc = nos_copy_to_user(out_cap_ptr, &cap, sizeof(cap));
        if (rc < 0) {
            task_del(child);
            return (long)rc;
        }
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

    rc = nos_cap_lookup(nos_task_active_cap_table(current),
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
        rc = nos_cap_lookup(nos_task_active_cap_table(current),
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
        rc = nos_cap_lookup(nos_task_active_cap_table(current),
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
    struct nos_ipc_msg kreq;
    struct nos_ipc_msg kresp;
    int rc;

    (void)a4;
    (void)a5;
    if (req_ptr != 0U) {
        rc = nos_copy_from_user(&kreq, req_ptr, sizeof(kreq));
        if (rc < 0) {
            return (long)rc;
        }
    }
    rc = nos_ipc_call((nos_handle_t)ep_cap,
                      (req_ptr != 0U) ? &kreq : NULL,
                      (resp_ptr != 0U) ? &kresp : NULL,
                      (uint32_t)timeout_ticks);
    if (rc == 0 && resp_ptr != 0U) {
        int cp_rc = nos_copy_to_user(resp_ptr, &kresp, sizeof(kresp));
        if (cp_rc < 0) {
            return (long)cp_rc;
        }
    }
    return (long)rc;
}

static long sys_nos_ipc_recv(uintptr_t ep_cap,
                             uintptr_t msg_ptr,
                             uintptr_t timeout_ticks,
                             uintptr_t a3,
                             uintptr_t a4,
                             uintptr_t a5)
{
    struct nos_ipc_msg kmsg;
    int rc;

    (void)a3;
    (void)a4;
    (void)a5;
    if (msg_ptr == 0U) {
        return -EINVAL;
    }
    rc = nos_ipc_recv((nos_handle_t)ep_cap, &kmsg, (uint32_t)timeout_ticks);
    if (rc == 0) {
        int cp_rc = nos_copy_to_user(msg_ptr, &kmsg, sizeof(kmsg));
        if (cp_rc < 0) {
            return (long)cp_rc;
        }
    }
    return (long)rc;
}

static long sys_nos_ipc_reply_wait(uintptr_t ep_cap,
                                   uintptr_t reply_ptr,
                                   uintptr_t next_req_ptr,
                                   uintptr_t timeout_ticks,
                                   uintptr_t a4,
                                   uintptr_t a5)
{
    struct nos_ipc_msg kreply;
    struct nos_ipc_msg knext;
    int rc;

    (void)a4;
    (void)a5;
    if (reply_ptr != 0U) {
        rc = nos_copy_from_user(&kreply, reply_ptr, sizeof(kreply));
        if (rc < 0) {
            return (long)rc;
        }
    }
    rc = nos_ipc_reply_wait((nos_handle_t)ep_cap,
                            (reply_ptr != 0U) ? &kreply : NULL,
                            (next_req_ptr != 0U) ? &knext : NULL,
                            (uint32_t)timeout_ticks);
    if (rc == 0 && next_req_ptr != 0U) {
        int cp_rc = nos_copy_to_user(next_req_ptr, &knext, sizeof(knext));
        if (cp_rc < 0) {
            return (long)cp_rc;
        }
    }
    return (long)rc;
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
    uint64_t kbits = 0ULL;
    int rc;

    (void)a3;
    (void)a4;
    (void)a5;
    if (out_bits_ptr == 0U) {
        return -EINVAL;
    }
    rc = nos_notify_wait((nos_handle_t)notif_cap, &kbits, (uint32_t)timeout_ticks);
    if (rc == 0) {
        int cp_rc = nos_copy_to_user(out_bits_ptr, &kbits, sizeof(kbits));
        if (cp_rc < 0) {
            return (long)cp_rc;
        }
    }
    return (long)rc;
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

    struct nos_cap_table *cap_tbl = nos_task_active_cap_table(current);

    switch (op) {
    case NOS_CAP_OP_DERIVE:
        rc = nos_cap_derive(cap_tbl,
                            (nos_handle_t)handle,
                            cap_tbl,
                            (nos_cap_rights_t)arg0,
                            (uint32_t)arg1,
                            &out_h);
        if (rc == 0 && out_ptr != 0U) {
            int cp_rc = nos_copy_to_user(out_ptr, &out_h, sizeof(out_h));
            if (cp_rc < 0) {
                return (long)cp_rc;
            }
        }
        return (rc < 0) ? (long)rc : (long)out_h;

    case NOS_CAP_OP_REVOKE:
        return (long)nos_cap_revoke(cap_tbl, (nos_handle_t)handle);

    case NOS_CAP_OP_INSPECT: {
        uint32_t ktype = 0U;
        nos_cap_rights_t krights = 0U;
        rc = nos_cap_inspect(cap_tbl, (nos_handle_t)handle, &ktype, &krights);
        if (rc == 0) {
            if (arg0 != 0U) {
                int cp0 = nos_copy_to_user(arg0, &ktype, sizeof(ktype));
                if (cp0 < 0) {
                    return (long)cp0;
                }
            }
            if (arg1 != 0U) {
                int cp1 = nos_copy_to_user(arg1, &krights, sizeof(krights));
                if (cp1 < 0) {
                    return (long)cp1;
                }
            }
        }
        return (long)rc;
    }

    case NOS_CAP_OP_CREATE_EP:
        if (nos_endpoint_create(&out_h) == NULL) {
            return -ENOMEM;
        }
        if (out_ptr != 0U) {
            int cp_rc = nos_copy_to_user(out_ptr, &out_h, sizeof(out_h));
            if (cp_rc < 0) {
                return (long)cp_rc;
            }
        }
        return (long)out_h;

    case NOS_CAP_OP_CREATE_NOTIF:
        if (nos_notification_create(&out_h) == NULL) {
            return -ENOMEM;
        }
        if (out_ptr != 0U) {
            int cp_rc = nos_copy_to_user(out_ptr, &out_h, sizeof(out_h));
            if (cp_rc < 0) {
                return (long)cp_rc;
            }
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

#ifdef CONFIG_NOS_AI_CORE
static long sys_nos_cell_control(uintptr_t op,
                                 uintptr_t cell_cap,
                                 uintptr_t arg0,
                                 uintptr_t arg1,
                                 uintptr_t out_ptr,
                                 uintptr_t a5)
{
    struct nos_cap_table *cap_tbl;
    struct nos_cell *target = NULL;
    int rc = 0;

    (void)a5;
    if (current == NULL) {
        return -EINVAL;
    }
    cap_tbl = nos_task_active_cap_table(current);

    switch (op) {
    case NOS_CELL_OP_CREATE:
    case NOS_CELL_OP_FORK: {
        struct nos_cell *parent = NULL;
        struct nos_cell *child;
        struct nos_cell_budget kbudget;
        nos_handle_t out_h = NOS_HANDLE_INVALID;
        bool parent_quota_full = false;

        if (op == NOS_CELL_OP_CREATE && cell_cap == NOS_HANDLE_INVALID && current->cell != NULL) {
            return -EACCES;
        }
        if (cell_cap != NOS_HANDLE_INVALID) {
            rc = nos_cap_lookup(cap_tbl,
                                (nos_handle_t)cell_cap,
                                NOS_CAP_CELL,
                                NOS_CAP_RIGHT_MANAGE | NOS_CAP_RIGHT_GRANT,
                                (void **)&parent,
                                NULL);
            if (rc < 0) {
                return (long)rc;
            }
        } else if (op == NOS_CELL_OP_FORK) {
            if (current->cell == NULL || !nos_object_get(&current->cell->header)) {
                return -EBADF;
            }
            parent = current->cell;
        }

        if (arg0 != 0U) {
            rc = nos_copy_from_user(&kbudget, arg0, sizeof(kbudget));
            if (rc < 0) {
                if (parent != NULL) {
                    (void)nos_object_put(&parent->header);
                }
                return (long)rc;
            }
        }
        child = (op == NOS_CELL_OP_FORK)
                    ? nos_cell_fork(parent, (arg0 != 0U) ? &kbudget : NULL)
                    : nos_cell_create(parent, (arg0 != 0U) ? &kbudget : NULL);
        if (parent != NULL) {
            parent_quota_full = (parent->state != NOS_CELL_STATE_ACTIVE ||
                                 parent->child_cells_used >= parent->budget.max_child_cells ||
                                 (op == NOS_CELL_OP_FORK &&
                                  ((parent->budget.cpu_ns_limit != 0ULL &&
                                    parent->cpu_ns_used >= parent->budget.cpu_ns_limit) ||
                                   (parent->budget.mem_bytes_limit != 0ULL &&
                                    parent->mem_bytes_used >= parent->budget.mem_bytes_limit) ||
                                   (parent->budget.tool_calls_limit != 0ULL &&
                                    parent->tool_calls_used >= parent->budget.tool_calls_limit) ||
                                   (parent->budget.accel_ops_limit != 0ULL &&
                                    parent->accel_ops_used >= parent->budget.accel_ops_limit))));
            (void)nos_object_put(&parent->header);
        }
        if (child == NULL) {
            return parent_quota_full ? -EDQUOT : -ENOMEM;
        }

        if (arg1 != 0U) {
            rc = nos_cell_map_telemetry(child, arg1);
            if (rc < 0) {
                (void)nos_cell_destroy(child);
                return (long)rc;
            }
        }

        rc = nos_cap_alloc(cap_tbl,
                           NOS_CAP_CELL,
                           child,
                           NOS_CAP_RIGHT_ALL,
                           child->cell_id,
                           &out_h);
        if (rc < 0) {
            (void)nos_cell_destroy(child);
            return (long)rc;
        }
        if (out_ptr != 0U) {
            rc = nos_copy_to_user(out_ptr, &out_h, sizeof(out_h));
            if (rc < 0) {
                return (long)rc;
            }
        }
        return (long)out_h;
    }

    case NOS_CELL_OP_DESTROY:
        rc = nos_cap_lookup(cap_tbl,
                            (nos_handle_t)cell_cap,
                            NOS_CAP_CELL,
                            NOS_CAP_RIGHT_MANAGE,
                            (void **)&target,
                            NULL);
        if (rc < 0) {
            return (long)rc;
        }
        (void)nos_cap_revoke_tree(cap_tbl, (nos_handle_t)cell_cap, NULL);
        rc = nos_cell_destroy(target);
        (void)nos_object_put(&target->header);
        return (long)rc;

    case NOS_CELL_OP_SET_BUDGET: {
        struct nos_cell_budget kbudget;
        if (arg0 == 0U) {
            return -EINVAL;
        }
        rc = nos_copy_from_user(&kbudget, arg0, sizeof(kbudget));
        if (rc < 0) {
            return (long)rc;
        }
        rc = nos_cap_lookup(cap_tbl,
                            (nos_handle_t)cell_cap,
                            NOS_CAP_CELL,
                            NOS_CAP_RIGHT_MANAGE,
                            (void **)&target,
                            NULL);
        if (rc < 0) {
            return (long)rc;
        }
        rc = nos_cell_set_budget(target, &kbudget);
        (void)nos_object_put(&target->header);
        return (long)rc;
    }

    case NOS_CELL_OP_GET_BUDGET: {
        struct nos_cell_budget kbudget;
        if (arg0 == 0U) {
            return -EINVAL;
        }
        rc = nos_cap_lookup(cap_tbl,
                            (nos_handle_t)cell_cap,
                            NOS_CAP_CELL,
                            NOS_CAP_RIGHT_READ,
                            (void **)&target,
                            NULL);
        if (rc < 0) {
            return (long)rc;
        }
        rc = nos_cell_get_budget(target, &kbudget);
        (void)nos_object_put(&target->header);
        if (rc == 0) {
            int cp_rc = nos_copy_to_user(arg0, &kbudget, sizeof(kbudget));
            if (cp_rc < 0) {
                return (long)cp_rc;
            }
        }
        return (long)rc;
    }

    case NOS_CELL_OP_READ_AUDIT: {
        uint32_t max_rec = (uint32_t)arg1;
        struct nos_audit_record *kbuf;
        if (arg0 == 0U || max_rec == 0U || max_rec > NOS_CELL_AUDIT_RING_CAPACITY) {
            return -EINVAL;
        }
        rc = nos_cap_lookup(cap_tbl,
                            (nos_handle_t)cell_cap,
                            NOS_CAP_CELL,
                            NOS_CAP_RIGHT_AUDIT,
                            (void **)&target,
                            NULL);
        if (rc < 0) {
            return (long)rc;
        }
        kbuf = kmalloc((u32)(sizeof(*kbuf) * max_rec), GFP_KERNEL);
        if (kbuf == NULL) {
            (void)nos_object_put(&target->header);
            return -ENOMEM;
        }
        rc = nos_cell_read_audit(target, kbuf, max_rec);
        (void)nos_object_put(&target->header);
        if (rc > 0) {
            int cp_rc = nos_copy_to_user(arg0, kbuf, (size_t)rc * sizeof(*kbuf));
            if (cp_rc < 0) {
                kfree(kbuf);
                return (long)cp_rc;
            }
        }
        kfree(kbuf);
        return (long)rc;
    }

    case NOS_CELL_OP_SNAPSHOT: {
        struct nos_cell_snapshot *snap;
        nos_handle_t out_h = NOS_HANDLE_INVALID;

        if ((nos_handle_t)cell_cap == NOS_HANDLE_INVALID) {
            if (current->cell == NULL || !nos_object_get(&current->cell->header)) {
                return -EBADF;
            }
            target = current->cell;
        } else {
            rc = nos_cap_lookup(cap_tbl,
                                (nos_handle_t)cell_cap,
                                NOS_CAP_CELL,
                                NOS_CAP_RIGHT_MANAGE,
                                (void **)&target,
                                NULL);
            if (rc < 0) {
                return (long)rc;
            }
        }

        snap = nos_cell_snapshot(target);
        (void)nos_object_put(&target->header);
        if (snap == NULL) {
            return -ENOMEM;
        }

        rc = nos_cap_alloc(cap_tbl,
                           NOS_CAP_SNAPSHOT,
                           snap,
                           NOS_CAP_RIGHT_ALL,
                           0U,
                           &out_h);
        if (rc < 0) {
            (void)nos_cell_snapshot_destroy(snap);
            return (long)rc;
        }
        if (out_ptr != 0U) {
            int cp_rc = nos_copy_to_user(out_ptr, &out_h, sizeof(out_h));
            if (cp_rc < 0) {
                return (long)cp_rc;
            }
        }
        return (long)out_h;
    }

    case NOS_CELL_OP_ROLLBACK: {
        struct nos_cell_snapshot *snap = NULL;

        if ((nos_handle_t)cell_cap == NOS_HANDLE_INVALID) {
            if (current->cell == NULL || !nos_object_get(&current->cell->header)) {
                return -EBADF;
            }
            target = current->cell;
        } else {
            rc = nos_cap_lookup(cap_tbl,
                                (nos_handle_t)cell_cap,
                                NOS_CAP_CELL,
                                NOS_CAP_RIGHT_MANAGE,
                                (void **)&target,
                                NULL);
            if (rc < 0) {
                return (long)rc;
            }
        }

        rc = nos_cap_lookup(cap_tbl,
                            (nos_handle_t)arg0,
                            NOS_CAP_SNAPSHOT,
                            NOS_CAP_RIGHT_READ,
                            (void **)&snap,
                            NULL);
        if (rc == 0) {
            rc = nos_cell_rollback(target, snap);
            (void)nos_object_put(&snap->header);
        }
        (void)nos_object_put(&target->header);
        return (long)rc;
    }

    case NOS_CELL_OP_BIND_NOTIF: {
        struct nos_notification *notif = NULL;

        if ((nos_handle_t)cell_cap == NOS_HANDLE_INVALID) {
            if (current->cell == NULL || !nos_object_get(&current->cell->header)) {
                return -EBADF;
            }
            target = current->cell;
        } else {
            rc = nos_cap_lookup(cap_tbl,
                                (nos_handle_t)cell_cap,
                                NOS_CAP_CELL,
                                NOS_CAP_RIGHT_MANAGE,
                                (void **)&target,
                                NULL);
            if (rc < 0) {
                return (long)rc;
            }
        }

        if ((nos_handle_t)arg0 != NOS_HANDLE_INVALID) {
            rc = nos_cap_lookup(cap_tbl,
                                (nos_handle_t)arg0,
                                NOS_CAP_NOTIFICATION,
                                NOS_CAP_RIGHT_SEND,
                                (void **)&notif,
                                NULL);
            if (rc < 0) {
                (void)nos_object_put(&target->header);
                return (long)rc;
            }
        }

        rc = nos_cell_bind_budget_notif(target, notif);
        if (notif != NULL) {
            (void)nos_object_put(&notif->object);
        }
        (void)nos_object_put(&target->header);
        return (long)rc;
    }

    default:
        return -EINVAL;
    }
}

static long sys_nos_vmo_create(uintptr_t args_ptr,
                               uintptr_t out_ptr,
                               uintptr_t a2,
                               uintptr_t a3,
                               uintptr_t a4,
                               uintptr_t a5)
{
    struct nos_vmo_create_args kargs;
    struct nos_vmo *vmo;
    nos_handle_t out_h = NOS_HANDLE_INVALID;
    int rc;

    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;

    if (current == NULL || args_ptr == 0U) {
        return -EINVAL;
    }

    rc = nos_copy_from_user(&kargs, args_ptr, sizeof(kargs));
    if (rc < 0) {
        return (long)rc;
    }
    vmo = nos_vmo_create(&kargs);
    if (vmo == NULL) {
        return (kargs.size_bytes == 0U || (kargs.size_bytes % 4096U) != 0U)
                   ? -EINVAL
                   : -ENOMEM;
    }

    rc = nos_cap_alloc(nos_task_active_cap_table(current),
                       NOS_CAP_VMO,
                       vmo,
                       NOS_CAP_RIGHT_ALL,
                       0U,
                       &out_h);
    if (rc < 0) {
        (void)nos_object_close(&vmo->header);
        (void)nos_object_retire(&vmo->header);
        return (long)rc;
    }

    if (out_ptr != 0U) {
        int cp_rc = nos_copy_to_user(out_ptr, &out_h, sizeof(out_h));
        if (cp_rc < 0) {
            return (long)cp_rc;
        }
    }
    return (long)out_h;
}

static int syscall_resolve_vmo_target(struct nos_cap_table *cap_tbl,
                                      nos_handle_t target_cap,
                                      struct nos_vm_space **out_space,
                                      struct nos_cell **out_charge_cell,
                                      struct nos_cell **out_pinned_cell)
{
    struct nos_cell *cell = NULL;
    struct nos_vm_space *space = NULL;
    int rc;

    *out_pinned_cell = NULL;

    if (target_cap == NOS_HANDLE_INVALID) {
        *out_space = nos_task_active_vm_space(current);
        *out_charge_cell = current->cell;
        return 0;
    }

    rc = nos_cap_lookup(cap_tbl,
                        target_cap,
                        NOS_CAP_CELL,
                        NOS_CAP_RIGHT_MAP | NOS_CAP_RIGHT_WRITE,
                        (void **)&cell,
                        NULL);
    if (rc == 0) {
        *out_space = nos_cell_vm_space(cell);
        *out_charge_cell = cell;
        *out_pinned_cell = cell;
        return 0;
    }

    rc = nos_cap_lookup(cap_tbl,
                        target_cap,
                        NOS_CAP_VM_SPACE,
                        NOS_CAP_RIGHT_WRITE,
                        (void **)&space,
                        NULL);
    if (rc < 0) {
        return rc;
    }
    *out_space = space;
    *out_charge_cell = current->cell;
    return 0;
}

static long sys_nos_vmo_control(uintptr_t op,
                                uintptr_t vmo_cap,
                                uintptr_t arg0,
                                uintptr_t arg1,
                                uintptr_t arg2,
                                uintptr_t arg3)
{
    struct nos_cap_table *cap_tbl;
    struct nos_vmo *vmo = NULL;
    struct nos_vmo *clone = NULL;
    struct nos_notification *notif = NULL;
    nos_cap_rights_t req_rights = 0U;
    nos_handle_t out_h = NOS_HANDLE_INVALID;
    int rc;

    if (current == NULL) {
        return -EINVAL;
    }
    cap_tbl = nos_task_active_cap_table(current);

    switch (op) {
    case NOS_VMO_OP_SEAL:
        req_rights = NOS_CAP_RIGHT_WRITE;
        break;
    case NOS_VMO_OP_CLONE_COW:
    case NOS_VMO_OP_CLONE_KV_PREFIX:
        req_rights = NOS_CAP_RIGHT_READ;
        break;
    case NOS_VMO_OP_BIND_EVICT_NOTIF:
        req_rights = NOS_CAP_RIGHT_MANAGE;
        break;
    case NOS_VMO_OP_SYNC_DMA:
        req_rights = NOS_CAP_RIGHT_READ;
        break;
    case NOS_VMO_OP_MAP:
        req_rights = NOS_CAP_RIGHT_MAP;
        if (((uint32_t)arg3 & HAL_VM_PROT_READ) != 0U) {
            req_rights |= NOS_CAP_RIGHT_READ;
        }
        if (((uint32_t)arg3 & HAL_VM_PROT_WRITE) != 0U) {
            req_rights |= NOS_CAP_RIGHT_WRITE;
        }
        if (((uint32_t)arg3 & HAL_VM_PROT_EXEC) != 0U) {
            req_rights |= NOS_CAP_RIGHT_EXEC;
        }
        break;
    case NOS_VMO_OP_UNMAP:
        req_rights = NOS_CAP_RIGHT_MAP;
        break;
    default:
        return -EINVAL;
    }

    rc = nos_cap_lookup(cap_tbl,
                        (nos_handle_t)vmo_cap,
                        NOS_CAP_VMO,
                        req_rights,
                        (void **)&vmo,
                        NULL);
    if (rc < 0) {
        return (long)rc;
    }

    switch (op) {
    case NOS_VMO_OP_SEAL:
        rc = nos_vmo_seal(vmo);
        (void)nos_object_put(&vmo->header);
        return (long)rc;

    case NOS_VMO_OP_CLONE_COW:
        clone = nos_vmo_clone_cow(vmo);
        (void)nos_object_put(&vmo->header);
        if (clone == NULL) {
            return -ENOMEM;
        }
        rc = nos_cap_alloc(cap_tbl, NOS_CAP_VMO, clone, NOS_CAP_RIGHT_ALL, 0U, &out_h);
        if (rc < 0) {
            (void)nos_object_close(&clone->header);
            (void)nos_object_retire(&clone->header);
            return (long)rc;
        }
        if (arg0 != 0U) {
            int cp_rc = nos_copy_to_user(arg0, &out_h, sizeof(out_h));
            if (cp_rc < 0) {
                return (long)cp_rc;
            }
        }
        return (long)out_h;

    case NOS_VMO_OP_CLONE_KV_PREFIX:
        clone = nos_vmo_clone_kv_prefix(vmo, (size_t)arg0);
        (void)nos_object_put(&vmo->header);
        if (clone == NULL) {
            return -EINVAL;
        }
        rc = nos_cap_alloc(cap_tbl, NOS_CAP_VMO, clone, NOS_CAP_RIGHT_ALL, 0U, &out_h);
        if (rc < 0) {
            (void)nos_object_close(&clone->header);
            (void)nos_object_retire(&clone->header);
            return (long)rc;
        }
        if (arg1 != 0U) {
            int cp_rc = nos_copy_to_user(arg1, &out_h, sizeof(out_h));
            if (cp_rc < 0) {
                return (long)cp_rc;
            }
        }
        return (long)out_h;

    case NOS_VMO_OP_BIND_EVICT_NOTIF:
        rc = nos_cap_lookup(cap_tbl,
                            (nos_handle_t)arg0,
                            NOS_CAP_NOTIFICATION,
                            NOS_CAP_RIGHT_SEND,
                            (void **)&notif,
                            NULL);
        if (rc == 0) {
            rc = nos_vmo_bind_evict_notif(vmo, notif);
            (void)nos_object_put(&notif->object);
        }
        (void)nos_object_put(&vmo->header);
        return (long)rc;

    case NOS_VMO_OP_SYNC_DMA:
        rc = nos_vmo_sync_dma(vmo, (size_t)arg0, (size_t)arg1, (uint32_t)arg2);
        (void)nos_object_put(&vmo->header);
        return (long)rc;

    case NOS_VMO_OP_MAP:
    case NOS_VMO_OP_UNMAP: {
        struct nos_vm_space *space = NULL;
        struct nos_cell *charge_cell = NULL;
        struct nos_cell *pinned_cell = NULL;

        rc = syscall_resolve_vmo_target(cap_tbl,
                                        (nos_handle_t)arg0,
                                        &space,
                                        &charge_cell,
                                        &pinned_cell);
        if (rc == 0) {
            if (op == NOS_VMO_OP_MAP) {
                rc = nos_vmo_map(vmo,
                                 space,
                                 arg1,
                                 0U,
                                 (size_t)arg2,
                                 (uint32_t)arg3 | HAL_VM_PROT_USER,
                                 charge_cell);
            } else {
                rc = nos_vmo_unmap(space, arg1, (size_t)arg2, charge_cell);
            }
        }
        if (pinned_cell != NULL) {
            (void)nos_object_put(&pinned_cell->header);
        }
        (void)nos_object_put(&vmo->header);
        return (long)rc;
    }

    default:
        (void)nos_object_put(&vmo->header);
        return -EINVAL;
    }
}

static long sys_nos_ring_create(uintptr_t args_ptr,
                                uintptr_t out_ptr,
                                uintptr_t a2,
                                uintptr_t a3,
                                uintptr_t a4,
                                uintptr_t a5)
{
    struct nos_ring_create_args kargs;
    uint64_t schemas[NOS_RING_MAX_SCHEMAS];
    struct nos_ring *ring;
    nos_handle_t out_h = NOS_HANDLE_INVALID;
    int rc;

    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;

    if (current == NULL || args_ptr == 0U) {
        return -EINVAL;
    }

    rc = nos_copy_from_user(&kargs, args_ptr, sizeof(kargs));
    if (rc < 0) {
        return (long)rc;
    }
    if (kargs.schema_count > NOS_RING_MAX_SCHEMAS) {
        return -EINVAL;
    }
    if (kargs.schema_count > 0U) {
        if (kargs.schema_list_ptr == 0ULL) {
            return -EINVAL;
        }
        rc = nos_copy_from_user(schemas,
                                (uintptr_t)kargs.schema_list_ptr,
                                (size_t)kargs.schema_count * sizeof(uint64_t));
        if (rc < 0) {
            return (long)rc;
        }
    }

    ring = nos_ring_create(&kargs,
                           (kargs.schema_count > 0U) ? schemas : NULL,
                           nos_task_active_vm_space(current),
                           current->cell);
    if (ring == NULL) {
        return -ENOMEM;
    }

    rc = nos_cap_alloc(nos_task_active_cap_table(current),
                       NOS_CAP_RING,
                       ring,
                       NOS_CAP_RIGHT_ALL,
                       0U,
                       &out_h);
    if (rc < 0) {
        (void)nos_ring_destroy(ring);
        return (long)rc;
    }

    if (out_ptr != 0U) {
        int cp_rc = nos_copy_to_user(out_ptr, &out_h, sizeof(out_h));
        if (cp_rc < 0) {
            return (long)cp_rc;
        }
    }
    return (long)out_h;
}

static long sys_nos_ring_enter(uintptr_t ring_cap,
                               uintptr_t to_submit,
                               uintptr_t min_complete,
                               uintptr_t flags,
                               uintptr_t aux4,
                               uintptr_t aux5)
{
    struct nos_ring *ring = NULL;
    nos_cap_rights_t req_rights = NOS_CAP_RIGHT_SEND;
    uint32_t f = (uint32_t)flags;
    int rc;

    if (current == NULL) {
        return -EINVAL;
    }

    if ((f & NOS_RING_ENTER_BIND_SERVER) != 0U) {
        req_rights = NOS_CAP_RIGHT_MANAGE | NOS_CAP_RIGHT_RECV;
    } else if ((f & (NOS_RING_ENTER_SERVER_POP | NOS_RING_ENTER_COMPLETE)) != 0U) {
        req_rights = NOS_CAP_RIGHT_RECV;
    }

    rc = nos_cap_lookup(nos_task_active_cap_table(current),
                        (nos_handle_t)ring_cap,
                        NOS_CAP_RING,
                        req_rights,
                        (void **)&ring,
                        NULL);
    if (rc < 0) {
        return (long)rc;
    }

    if ((f & NOS_RING_ENTER_BIND_SERVER) != 0U) {
        rc = nos_ring_bind_server(ring, current, NULL, NULL);
        if (rc < 0 || f == NOS_RING_ENTER_BIND_SERVER) {
            (void)nos_object_put(&ring->header);
            return (long)rc;
        }
    }

    if ((f & NOS_RING_ENTER_COMPLETE) != 0U) {
        struct nos_ring_cqe kcqe;
        if (aux4 == 0U) {
            (void)nos_object_put(&ring->header);
            return -EINVAL;
        }
        rc = nos_copy_from_user(&kcqe, aux4, sizeof(kcqe));
        if (rc == 0) {
            rc = nos_ring_server_complete(ring, &kcqe);
        }
        (void)nos_object_put(&ring->header);
        return (long)rc;
    }

    if ((f & NOS_RING_ENTER_SERVER_POP) != 0U) {
        uint32_t max_pop = (to_submit > 0U && to_submit <= NOS_RING_DEFAULT_ENTRIES)
                               ? (uint32_t)to_submit
                               : 1U;
        uint32_t popped = 0U;
        uint32_t pop_tmo = (uint32_t)aux5;

        if (aux4 == 0U) {
            (void)nos_object_put(&ring->header);
            return -EINVAL;
        }
        while (popped < max_pop) {
            struct nos_ring_sqe ksqe;
            int prc = nos_ring_server_pop(
                ring, &ksqe, (popped == 0U) ? pop_tmo : NOS_IPC_TIMEOUT_NONBLOCK);
            if (prc < 0) {
                if (popped > 0U && (prc == -EAGAIN || prc == -ETIMEDOUT)) {
                    rc = (int)popped;
                } else {
                    rc = prc;
                }
                break;
            }
            rc = nos_copy_to_user(aux4 + (uintptr_t)popped * sizeof(ksqe),
                                  &ksqe, sizeof(ksqe));
            if (rc < 0) {
                break;
            }
            popped++;
            rc = (int)popped;
        }
        (void)nos_object_put(&ring->header);
        return (long)rc;
    }

    rc = nos_ring_enter_with_cap(ring,
                                 current,
                                 (nos_handle_t)ring_cap,
                                 (uint32_t)to_submit,
                                 (uint32_t)min_complete,
                                 f,
                                 (uint32_t)aux4);
    (void)nos_object_put(&ring->header);
    return (long)rc;
}

static long sys_nos_sched_infer(uintptr_t op,
                                uintptr_t target_cap,
                                uintptr_t arg,
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

    if (op == NOS_SCHED_OP_QUERY_TOPO) {
        struct nos_cpu_topo_info topo;
        if (arg == 0U) {
            return -EINVAL;
        }
        rc = sched_infer_query_topo(&topo);
        if (rc == 0) {
            int cp_rc = nos_copy_to_user(arg, &topo, sizeof(topo));
            if (cp_rc < 0) {
                return (long)cp_rc;
            }
        }
        return (long)rc;
    }

    if (op == NOS_SCHED_OP_GANG_WAKEUP) {
        uint32_t gid = (arg != 0U) ? (uint32_t)arg : (uint32_t)target_cap;
        return (long)sched_infer_gang_wakeup(gid);
    }

    if ((nos_handle_t)target_cap == NOS_HANDLE_INVALID) {
        if (!task_get(current)) {
            return -EINVAL;
        }
        target = current;
    } else {
        rc = nos_cap_lookup(nos_task_active_cap_table(current),
                            (nos_handle_t)target_cap,
                            NOS_CAP_TASK,
                            NOS_CAP_RIGHT_MANAGE,
                            (void **)&target,
                            NULL);
        if (rc < 0) {
            return (long)rc;
        }
        if (!task_get(target)) {
            return -ESTALE;
        }
    }

    switch (op) {
    case NOS_SCHED_OP_SET_INFER: {
        struct nos_infer_sched_param kparam;
        if (arg == 0U) {
            rc = -EINVAL;
            break;
        }
        rc = nos_copy_from_user(&kparam, arg, sizeof(kparam));
        if (rc == 0) {
            rc = sched_infer_set_param(target, &kparam);
        }
        break;
    }
    case NOS_SCHED_OP_GANG_JOIN:
        rc = sched_infer_gang_join(target, (uint32_t)arg);
        break;
    case NOS_SCHED_OP_GANG_LEAVE:
        sched_infer_gang_leave(target);
        rc = 0;
        break;
    default:
        rc = -EINVAL;
        break;
    }

    (void)task_put(target);
    return (long)rc;
}

static long sys_nos_accel_submit(uintptr_t a0, uintptr_t a1, uintptr_t a2,
                                 uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
    uint32_t op = (uint32_t)a0;
    nos_handle_t accel_cap = (nos_handle_t)a1;
    struct nos_accel_device *dev = NULL;
    struct nos_cap_table *cap_tbl;
    int rc = 0;

    (void)a5;

    if (current == NULL) {
        return -EINVAL;
    }

    cap_tbl = nos_task_active_cap_table(current);
    if (accel_cap != NOS_HANDLE_INVALID) {
        nos_cap_rights_t need = (op == NOS_ACCEL_OP_QUERY_CAPS) ?
                                NOS_CAP_RIGHT_READ : NOS_CAP_RIGHT_SEND;
        void *obj = NULL;
        if (cap_tbl == NULL) {
            return -EBADF;
        }
        rc = nos_cap_lookup(cap_tbl, accel_cap, NOS_CAP_ACCEL, need, &obj, NULL);
        if (rc < 0) {
            return (long)rc;
        }
        dev = (struct nos_accel_device *)obj;
    } else if (op == NOS_ACCEL_OP_QUERY_CAPS) {
        dev = nos_accel_get_default();
        if (dev == NULL) {
            return -ENODEV;
        }
    } else {
        return -EBADF;
    }

    switch (op) {
    case NOS_ACCEL_OP_QUERY_CAPS: {
        struct nos_accel_caps kcaps = { 0 };
        if (a2 == 0U) {
            rc = -EINVAL;
            break;
        }
        rc = nos_accel_query_caps(dev, &kcaps);
        if (rc == 0) {
            int cp_rc = nos_copy_to_user(a2, &kcaps, sizeof(kcaps));
            if (cp_rc < 0) {
                rc = cp_rc;
            }
        }
        break;
    }
    case NOS_ACCEL_OP_BIND_VMO:
        rc = nos_accel_bind_vmo(dev, current, (nos_handle_t)a2);
        break;
    case NOS_ACCEL_OP_SUBMIT: {
        struct nos_accel_job_desc kdesc;
        struct nos_accel_job_result kres = { 0 };
        if (a2 == 0U) {
            rc = -EINVAL;
            break;
        }
        rc = nos_copy_from_user(&kdesc, a2, sizeof(kdesc));
        if (rc == 0) {
            rc = nos_accel_submit(dev, current, &kdesc, &kres);
            if (a3 != 0U) {
                int cp_rc = nos_copy_to_user(a3, &kres, sizeof(kres));
                if (cp_rc < 0 && rc == 0) {
                    rc = cp_rc;
                }
            }
        }
        break;
    }
    case NOS_ACCEL_OP_WAIT: {
        uint64_t req_id = (uint64_t)a2;
        uint32_t timeout_ms = (uint32_t)a4;
        struct nos_accel_job_result kres = { 0 };
        rc = nos_accel_wait_job(dev, current, req_id, &kres, timeout_ms);
        if (a3 != 0U && rc != -EAGAIN && rc != -ETIMEDOUT) {
            int cp_rc = nos_copy_to_user(a3, &kres, sizeof(kres));
            if (cp_rc < 0 && rc == 0) {
                rc = cp_rc;
            }
        }
        break;
    }
    case NOS_ACCEL_OP_CANCEL:
        rc = nos_accel_cancel_job(dev, (uint64_t)a2);
        break;
    case NOS_ACCEL_OP_RESET_CTX:
        rc = nos_accel_reset_ctx(dev, current, (uint32_t)a2);
        break;
    default:
        rc = -EINVAL;
        break;
    }

    (void)nos_object_put(&dev->header);
    return (long)rc;
}
#else
static long sys_nos_cell_control(uintptr_t a0, uintptr_t a1, uintptr_t a2,
                                 uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return -ENOSYS;
}

static long sys_nos_vmo_create(uintptr_t a0, uintptr_t a1, uintptr_t a2,
                               uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return -ENOSYS;
}

static long sys_nos_vmo_control(uintptr_t a0, uintptr_t a1, uintptr_t a2,
                                uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return -ENOSYS;
}

static long sys_nos_ring_create(uintptr_t a0, uintptr_t a1, uintptr_t a2,
                                uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return -ENOSYS;
}

static long sys_nos_ring_enter(uintptr_t a0, uintptr_t a1, uintptr_t a2,
                               uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return -ENOSYS;
}

static long sys_nos_sched_infer(uintptr_t a0, uintptr_t a1, uintptr_t a2,
                                uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return -ENOSYS;
}

static long sys_nos_accel_submit(uintptr_t a0, uintptr_t a1, uintptr_t a2,
                                 uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return -ENOSYS;
}
#endif

static long sys_nos_cap_revoke_tree(uintptr_t handle,
                                    uintptr_t out_revoked_ptr,
                                    uintptr_t a2,
                                    uintptr_t a3,
                                    uintptr_t a4,
                                    uintptr_t a5)
{
    uint32_t revoked = 0U;
    int rc;

    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;

    if (current == NULL) {
        return -EINVAL;
    }

    rc = nos_cap_revoke_tree(nos_task_active_cap_table(current),
                             (nos_handle_t)handle,
                             &revoked);
    if (rc == 0 && out_revoked_ptr != 0U) {
        int cp_rc = nos_copy_to_user(out_revoked_ptr, &revoked, sizeof(revoked));
        if (cp_rc < 0) {
            return (long)cp_rc;
        }
    }
    return (rc < 0) ? (long)rc : (long)revoked;
}

static const nos_syscall_fn_t g_syscall_table[SYS_NOS_MAX] = {
    [SYS_NOS_YIELD]           = sys_nos_yield,
    [SYS_NOS_EXIT]            = sys_nos_exit,
    [SYS_NOS_DEBUG_CONSOLE]   = sys_nos_debug_console,
    [SYS_NOS_CLOCK_GET]       = sys_nos_clock_get,
    [SYS_NOS_SLEEP_UNTIL]     = sys_nos_sleep_until,
    [SYS_NOS_THREAD_CREATE]   = sys_nos_thread_create,
    [SYS_NOS_THREAD_CONTROL]  = sys_nos_thread_control,
    [SYS_NOS_VM_MAP]          = sys_nos_vm_map,
    [SYS_NOS_VM_UNMAP]        = sys_nos_vm_unmap,
    [SYS_NOS_IPC_CALL]        = sys_nos_ipc_call,
    [SYS_NOS_IPC_RECV]        = sys_nos_ipc_recv,
    [SYS_NOS_IPC_REPLY_WAIT]  = sys_nos_ipc_reply_wait,
    [SYS_NOS_NOTIFY_SIGNAL]   = sys_nos_notify_signal,
    [SYS_NOS_NOTIFY_WAIT]     = sys_nos_notify_wait,
    [SYS_NOS_CAP_CONTROL]     = sys_nos_cap_control,
    [SYS_NOS_IRQ_CONTROL]     = sys_nos_irq_control,
    [SYS_NOS_CELL_CONTROL]    = sys_nos_cell_control,
    [SYS_NOS_VMO_CREATE]      = sys_nos_vmo_create,
    [SYS_NOS_VMO_CONTROL]     = sys_nos_vmo_control,
    [SYS_NOS_RING_CREATE]     = sys_nos_ring_create,
    [SYS_NOS_RING_ENTER]      = sys_nos_ring_enter,
    [SYS_NOS_SCHED_INFER]     = sys_nos_sched_infer,
    [SYS_NOS_CAP_REVOKE_TREE] = sys_nos_cap_revoke_tree,
    [SYS_NOS_ACCEL_SUBMIT]    = sys_nos_accel_submit,
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
