/* Copyright (C) 2026 NOS contributors */
#include "kernel.h"
#include "elf.h"
static struct spinlock kernel_lock;
static unsigned device_service_caps(unsigned caps)
{
#ifdef NOS_LINUX_COMPAT
    caps |= NOS_CAP_SHARED_BROKER | NOS_CAP_PROCESS_QUERY;
#endif
    return (caps & ~(NOS_CAP_SPAWN | NOS_CAP_IPC)) | NOS_CAP_DEVICE;
}
/* All callers hold kernel_lock, so task and fork-reference state agree. */
static unsigned group_state(unsigned group)
{
    unsigned state = ipc_group_fork_references(group);
    for (unsigned i = 0; i < NOS_TASKS; i++)
        if (tasks[i].space_owner == group && tasks[i].state != TASK_UNUSED &&
            tasks[i].state != TASK_DEAD && tasks[i].state != TASK_RETIRED) {
            state |= NOS_GROUP_LIVE_TASKS; break;
        }
    return state;
}
/* Keep the bounded 512-byte scratch out of unrelated dispatch stack frames. */
__attribute__((noinline)) static intptr_t group_query_batch(uintptr_t buffer, uintptr_t count)
{
    if (!count || count > NOS_GROUP_QUERY_MAX) return -NOS_EINVAL;
    struct nos_group_state entries[NOS_GROUP_QUERY_MAX];
    size_t bytes = count * sizeof(entries[0]);
    if (user_copy_in(entries, buffer, bytes) || !user_access(current_task, buffer, bytes, 1)) return -NOS_EFAULT;
    for (unsigned i = 0; i < count; i++) if (entries[i].group > INT32_MAX) return -NOS_EINVAL;
    for (unsigned i = 0; i < count; i++) entries[i].state = (int32_t)group_state(entries[i].group);
    return user_copy_out(buffer, entries, bytes);
}
static intptr_t dispatch(uintptr_t nr, uintptr_t a, uintptr_t b, uintptr_t c)
{
#if defined(NOS_MMU) && defined(NOS_LINUX_COMPAT)
    if (current_task->abi == NOS_ABI_GATEWAY && current_task->signal_pending & ~current_task->signal_mask &&
        (nr == NOS_SYS_CALL || nr == NOS_SYS_CALL_EX ||
         (nr == NOS_SYS_IPC_VECTOR && a == NOS_IPC_VECTOR_CALL) ||
         nr == NOS_SYS_SLEEP || nr == NOS_SYS_FUTEX_WAIT))
        return -NOS_EINTR;
#endif
    switch (nr) {
    case NOS_SYS_WRITE: {
        char buffer[128];
        if (b > 1024) return -NOS_EINVAL;
        if (!user_access(current_task, a, b, 0)) return -NOS_EFAULT;
        size_t left = b;
        while (left) {
            size_t n = left > sizeof(buffer) ? sizeof(buffer) : left;
            if (user_copy_in(buffer, a, n)) return -NOS_EFAULT;
            for (size_t i = 0; i < n; i++) arch_putc(buffer[i]);
            a += n; left -= n;
        }
        return (intptr_t)b;
    }
    case NOS_SYS_GETPID: return current_task->id;
    case NOS_SYS_CREDENTIALS: return credentials_operation(a, b, c);
    case NOS_SYS_GROUP_ALIVE:
        if (!(current_task->caps & NOS_CAP_PROCESS_QUERY)) return -NOS_EACCES;
        if (c == NOS_GROUP_QUERY_BATCH) return group_query_batch(a, b);
        if (a > INT32_MAX || b || c) return -NOS_EINVAL;
        return (intptr_t)group_state((unsigned)a);
    case NOS_SYS_YIELD: reschedule = 1; return 0;
    case NOS_SYS_EXIT:
        if (b && c) (void)vm_unmap(current_task, b, c);
        task_finish(current_task, (intptr_t)a); return 0;
    case NOS_SYS_SLEEP:
        if (a > 10000) return -NOS_EINVAL;
        current_task->wake_tick = kernel_ticks + (a ? a : 1);
        current_task->state = TASK_SLEEP; reschedule = 1; return 0;
    case NOS_SYS_TICKS: return (intptr_t)kernel_ticks;
    case NOS_SYS_TICKS64: return user_copy_out(a, &kernel_ticks, sizeof(kernel_ticks));
    case NOS_SYS_CLOCK_GET64: {
        if (c) return -NOS_EINVAL;
        struct nos_clock_value value; int rc = kernel_clock_snapshot(a, &value);
        return rc ? rc : user_copy_out(b, &value, sizeof(value));
    }
    case NOS_SYS_CAPS:
#if defined(NOS_SELFTEST) && defined(NOS_KERNEL_FAULT_INJECT) && NOS_KERNEL_FAULT_INJECT
        arch_fault_probe();
        kernel_fail("fault injection returned");
#endif
        return current_task->caps;
    case NOS_SYS_FEATURES: return current_task->caps & NOS_FEATURE_MASK;
    case NOS_SYS_PERMISSIONS: return current_task->caps & NOS_PERMISSION_MASK;
    case NOS_SYS_PRIORITY:
        if (a > 1) return -NOS_EINVAL;
        return a ? current_task->base_priority : current_task->priority;
    case NOS_SYS_SPAWN: return task_spawn(a, b, c);
    case NOS_SYS_WAIT: return task_wait(a, b, c);
    case NOS_SYS_LINUX_WAITCONTROL: return linux_wait_control(a, b, c);
    case NOS_SYS_LINUX_EXEC: return linux_exec_current(a, b, c);
    case NOS_SYS_SHARED: {
#if defined(NOS_MMU) && defined(NOS_LINUX_COMPAT)
        struct nos_shared_request request;
        if (c) return -NOS_EINVAL;
        if (user_copy_in(&request, b, sizeof(request))) return -NOS_EFAULT;
        if (request.handle > UINT32_MAX) return -NOS_EINVAL;
        if (request.prot > 7 || request.flags > UINT32_MAX) {
            if (a == NOS_SHARED_MAP) return vm_shared_map(current_task, (unsigned)request.handle, request.address,
                request.length, request.offset, UINT32_MAX, UINT32_MAX);
            return -NOS_EINVAL;
        }
        if (a == NOS_SHARED_CREATE) return vm_shared_create(current_task, request.length);
        if (a == NOS_SHARED_GRANT) {
            struct task *target = find_task(request.target);
            if (!target || target->state == TASK_DEAD) return -NOS_ESRCH;
            return vm_shared_grant(current_task, (unsigned)request.handle, target, (unsigned)request.prot);
        }
        if (a == NOS_SHARED_MAP) return vm_shared_map(current_task, (unsigned)request.handle, request.address,
            request.length, request.offset, (unsigned)request.prot, (unsigned)request.flags);
        if (a == NOS_SHARED_RELEASE) return vm_shared_release(current_task, (unsigned)request.handle);
        return -NOS_EINVAL;
#else
        return -NOS_ENOTSUP;
#endif
    }
    case NOS_SYS_CALL: return ipc_call(a, b, c, 0);
    case NOS_SYS_IPC_VECTOR: return ipc_vector(a, b, c);
    case NOS_SYS_RECV: return ipc_recv(a, b);
    case NOS_SYS_REPLY: return ipc_reply(a, b);
    case NOS_SYS_IPC_XFER: return b || c ? -NOS_EINVAL : ipc_xfer(a);
    case NOS_SYS_IPC_SUBJECT: return b || c ? -NOS_EINVAL : ipc_subject(a);
    case NOS_SYS_IPC_LIMITS: return b || c ? -NOS_EINVAL : ipc_limits(a);
    case NOS_SYS_MMAP: return a > UINT32_MAX ? -NOS_EINVAL : vm_map(current_task, (unsigned)a, b);
    case NOS_SYS_MUNMAP: return vm_unmap(current_task, a, b);
    case NOS_SYS_MPROTECT: return b > UINT32_MAX ? -NOS_EINVAL : vm_protect(current_task, a, (unsigned)b, c);
    case NOS_SYS_BRK:
        return vm_brk(current_task, a);
    case NOS_SYS_MUTEX_CREATE: case NOS_SYS_MUTEX_LOCK:
    case NOS_SYS_MUTEX_UNLOCK: case NOS_SYS_MUTEX_DESTROY:
        return mutex_operation((unsigned)nr, a);
    case NOS_SYS_TLS_GET: return (intptr_t)current_task->tls_value;
    case NOS_SYS_TLS_STORAGE:
#ifdef NOS_MMU
        return -NOS_ENOTSUP;
#else
        return (intptr_t)current_task->tls_memory;
#endif
    case NOS_SYS_TLS_BIND:
        if ((a & (sizeof(uintptr_t) - 1)) || !user_access(current_task, a, sizeof(uintptr_t), 1) ||
            !user_access(current_task, b, 1, 0)) return -NOS_EFAULT;
        current_task->tls_slot = a; current_task->tls_value = b;
        return user_copy_out(a, &b, sizeof(b));
    case NOS_SYS_USER_CHECK:
        if (c != NOS_PROT_READ && c != NOS_PROT_WRITE) return -NOS_EINVAL;
        return user_access(current_task, a, b, c == NOS_PROT_WRITE) ? 0 : -NOS_EFAULT;
    case NOS_SYS_USER_COPY: {
        if (!user_access(current_task, a, c, 1) || !user_access(current_task, b, c, 0)) return -NOS_EFAULT;
        unsigned char buffer[128];
        /* memmove order is needed for overlapping buffers. */
        if (a > b && a - b < c) {
            while (c) { size_t n = c < sizeof(buffer) ? c : sizeof(buffer); c -= n;
                if (user_copy_in(buffer, b + c, n) || user_copy_out(a + c, buffer, n)) return -NOS_EFAULT; }
        } else {
            while (c) { size_t n = c < sizeof(buffer) ? c : sizeof(buffer);
                if (user_copy_in(buffer, b, n) || user_copy_out(a, buffer, n)) return -NOS_EFAULT;
                a += n; b += n; c -= n; }
        }
        return 0;
    }
    case NOS_SYS_ABI_QUERY: {
        struct nos_abi_info info = { NOS_ABI_VERSION, sizeof(uintptr_t) * 8, sizeof(struct nos_message), NOS_PAGE_SIZE };
        return user_copy_out(a, &info, sizeof(info));
    }
    case NOS_SYS_ENDPOINT_CREATE: return endpoint_create();
    case NOS_SYS_ENDPOINT_CLOSE: return endpoint_close(a);
    case NOS_SYS_ENDPOINT_GRANT: return endpoint_grant(a, b, c, 0);
    case NOS_SYS_ENDPOINT_REVOKE: return endpoint_grant(a, b, c, 1);
    case NOS_SYS_CALL_EX: {
        struct nos_call_args args;
        if (user_copy_in(&args, a, sizeof(args))) return -NOS_EFAULT;
        return ipc_call(args.endpoint, args.request, args.reply, args.timeout_ticks);
    }
    case NOS_SYS_IPC_CANCEL: return ipc_cancel(a);
    case NOS_SYS_THREAD_CREATE: return thread_create(a, b);
    case NOS_SYS_SET_TID_ADDRESS:
        if (a && ((a & 3) || !user_access(current_task, a, sizeof(unsigned), 1))) return -NOS_EFAULT;
        current_task->clear_tid = a; return current_task->id;
    case NOS_SYS_FUTEX_WAIT: return futex_wait(a, b, c);
    case NOS_SYS_FUTEX_WAKE: return futex_wake(a, b);
    case NOS_SYS_FUTEX_REQUEUE: return futex_requeue(a);
    case NOS_SYS_TASK_CANCEL: {
        struct task *peer = find_task(a);
        if (!peer || (peer->state == TASK_DEAD && !(b & NOS_CANCEL_GROUP))) return -NOS_ESRCH;
        if (!(current_task->caps & NOS_CAP_SPAWN) && peer->parent != current_task->id) return -NOS_EACCES;
        if (peer == current_task || peer->id % NOS_TASKS >= NOS_IDLE_FIRST) return -NOS_EINVAL;
        if (b & ~(uintptr_t)NOS_CANCEL_GROUP) return -NOS_EINVAL;
        if (b & NOS_CANCEL_GROUP) {
            unsigned group = peer->space_owner;
            if (group == current_task->space_owner) return -NOS_EINVAL;
            for (unsigned i = 0; i < NOS_IDLE_FIRST; i++) {
                struct task *member = &tasks[i];
                if (member->space_owner != group || member->state == TASK_UNUSED || member->state == TASK_DEAD || member->state == TASK_RETIRED) continue;
#ifdef NOS_MMU
                if (member->abi != NOS_ABI_NATIVE) member->linux_exit_signal = 9;
#endif
                if (member->run_cpu != UINT32_MAX && member->run_cpu != arch_cpu_id()) member->cancel_pending = 1;
                else task_finish(member, -NOS_EINTR);
            }
            return 0;
        }
        if (peer->run_cpu != UINT32_MAX && peer->run_cpu != arch_cpu_id()) {
            peer->cancel_pending = 1; return 0;
        }
        task_finish(peer, -NOS_EINTR); return 0;
    }
    case NOS_SYS_SERVICE_RESTART:
#if defined(NOS_NK60_V2) || defined(NOS_ESP_IDF)
        /* MCU profiles do not expose the native-stack service restart path. */
        return -NOS_ENOTSUP;
#else
        if (!(current_task->caps & NOS_CAP_SPAWN) || a) return -NOS_EACCES;
        if (tasks[0].state != TASK_DEAD && tasks[0].state != TASK_UNUSED) return -NOS_EBUSY;
        for (unsigned i = 2; i < NOS_IDLE_FIRST; i++) {
            struct task *peer = &tasks[i];
            if (peer->space_owner == tasks[0].space_owner && peer->state != TASK_UNUSED &&
                peer->state != TASK_DEAD && peer->state != TASK_RETIRED) return -NOS_EBUSY;
        }
        if (tasks[0].state == TASK_DEAD) task_reap(&tasks[0]);
        if (create_task(0, (uintptr_t)nos_vfs_server, 0, 4,
            device_service_caps(current_task->caps), UINT32_MAX) < 0) return -NOS_ENOMEM;
        ipc_init(); reschedule = 1; return tasks[0].id;
#endif
    case NOS_SYS_OBJECT_STATS: {
        struct nos_object_stats stats = { 0, mutex_count(), endpoint_count(), page_available() };
        for (unsigned i = 0; i < NOS_TASKS; i++) stats.tasks += tasks[i].state != TASK_UNUSED && tasks[i].state != TASK_RETIRED;
        return user_copy_out(a, &stats, sizeof(stats));
    }
    case NOS_SYS_GROUP_ID: return current_task->space_owner;
    case NOS_SYS_EXIT_GROUP: {
        unsigned group = current_task->space_owner;
        for (unsigned i = 0; i < NOS_TASKS; i++) {
            struct task *peer = &tasks[i];
            if (peer == current_task || peer->state == TASK_UNUSED || peer->state == TASK_DEAD ||
                peer->state == TASK_RETIRED || peer->space_owner != group) continue;
            if (peer->run_cpu != UINT32_MAX && peer->run_cpu != arch_cpu_id()) peer->cancel_pending = 1;
            else task_finish(peer, (intptr_t)a);
        }
        task_finish(current_task, (intptr_t)a); return 0;
    }
    case NOS_SYS_TASK_AUTHORIZE: {
        struct task *peer = find_task(a);
        if (!peer) return -NOS_ESRCH;
        unsigned allowed = NOS_CAP_SPAWN | NOS_CAP_IPC;
#ifdef NOS_SELFTEST
        /* Initial init may grant a test observer access to lifecycle state.
         * This observation privilege is never grantable in production. */
        if (current_task->id == 1 && (current_task->caps & NOS_CAP_SPAWN)) allowed |= NOS_CAP_PROCESS_QUERY;
#endif
        if (!(current_task->caps & NOS_CAP_SPAWN) || b & ~(uintptr_t)allowed) return -NOS_EACCES;
        peer->caps |= (unsigned)b; return 0;
    }
    case NOS_SYS_CPU_QUERY:
        return a == 0 ? (intptr_t)arch_cpu_id() : a == 1 ? (intptr_t)arch_cpu_count() : -NOS_EINVAL;
    case NOS_SYS_OBJECT_GRANT: case NOS_SYS_OBJECT_REVOKE:
        return (b >= NOS_ENDPOINT_BASE || (b == 0 && c != NOS_RIGHT_MUTEX)) ? endpoint_grant(a, b, c, nr == NOS_SYS_OBJECT_REVOKE) :
            mutex_grant(a, b, c, nr == NOS_SYS_OBJECT_REVOKE);
    case NOS_SYS_IRQ_BIND: case NOS_SYS_IRQ_WAIT: case NOS_SYS_IRQ_ACK: case NOS_SYS_IRQ_RELEASE:
        return irq_operation((unsigned)nr, a, b);
    case NOS_SYS_DEVICE:
        if (!(current_task->caps & NOS_CAP_DEVICE)) return -NOS_EACCES;
        return arch_device_operation(current_task, a, b, c);
    case NOS_SYS_BOOTFILE: return bootfile_operation(a, b);
    case NOS_SYS_EXEC_LINUX: return task_exec_linux(a, b, c);
    case NOS_SYS_LINUX_RETURN: return linux_gateway_return(a);
    case NOS_SYS_LINUX_RAISE:
#if defined(NOS_MMU) && defined(NOS_LINUX_COMPAT)
        if (current_task->abi != NOS_ABI_GATEWAY || !current_task->linux_gateway_active) return -NOS_EPERM;
        if (!a || a > 64 || b || c) return -NOS_EINVAL;
        /* Gateway cleanup must finish before handlers use the Linux stack.
         * linux_signal_deliver defers all delivery until RETURN restores ABI. */
        return linux_signal_send(current_task, (unsigned)a, 0, current_task->space_owner, 0);
#else
        return -NOS_EPERM;
#endif
    case NOS_SYS_LINUX_MAP: {
#if defined(NOS_MMU) && defined(NOS_LINUX_COMPAT)
        if (current_task->abi != NOS_ABI_GATEWAY) return -NOS_EPERM;
        struct nos_linux_map request;
        if (user_copy_in(&request, a, sizeof(request))) return -NOS_EFAULT;
        if (request.prot > 7 || request.flags > UINT32_MAX) return -NOS_EINVAL;
        return vm_map_linux(current_task, request.address, request.length,
                            (unsigned)request.prot, (unsigned)request.flags);
#else
        return -NOS_ENOTSUP;
#endif
    }
    case NOS_SYS_EXEC_SPAWN: {
        if (!(current_task->caps & NOS_CAP_SPAWN)) return -NOS_EACCES;
        struct nos_exec_request request;
#ifdef NOS_LINUX_COMPAT
        static
#endif
        struct nos_exec_args args;
        if (user_copy_in(&request, a, sizeof(request))) return -NOS_EFAULT;
        if (request.priority > 30) return -NOS_EINVAL;
        int rc = nos_exec_copy_args(&request, &args); if (rc) return rc;
        size_t bytes; const void *image = nos_initramfs_find(args.path, &bytes);
        if (!image) return -NOS_ENOENT;
        rc = process_nproc_admit(current_task); if (rc) return rc;
        for (unsigned i = 2; i < NOS_IDLE_FIRST; i++) {
            if (tasks[i].state != TASK_UNUSED) continue;
            struct task *t = &tasks[i];
#ifdef NOS_LINUX_COMPAT
            if (t->credential_ref) kernel_fail("exec spawn with credentials");
#endif
            unsigned generation = t->generation;
            if (generation > (NOS_TID_MAX - i) / NOS_TASKS)
                kernel_fail("task identifier exhausted");
            k_memset(t, 0, sizeof(*t));
            t->id = generation * NOS_TASKS + i; t->generation = generation;
            t->space_id = NOS_SPACES; t->space_owner = t->id; t->run_cpu = t->pin_cpu = UINT32_MAX;
            t->priority = t->base_priority = request.priority;
            t->parent = current_task->id; t->caps = current_task->caps & ~NOS_CAP_SPAWN;
            rc = credentials_create(t, current_task); if (rc) return rc;
            if (space_create_empty(t)) { space_destroy(t); credentials_release(t); return -NOS_ENOMEM; }
            struct nos_elf_result result;
            rc = nos_elf_load(t, image, bytes, &args, &result);
            if (rc) { space_destroy(t); credentials_release(t); return rc; }
            arch_frame_init(&t->frame, result.entry, result.stack, result.argc, result.argv);
#if defined(NOS_ARM64)
            t->frame.tls = result.tls;
#elif defined(NOS_RISCV)
            t->frame.x[4] = result.tls;
#endif
            ipc_task_inherit(t, current_task); t->state = TASK_READY;
            reschedule = 1; return t->id;
        }
        return -NOS_EAGAIN;
    }
#ifdef NOS_SELFTEST
    case NOS_SYS_FINISH:
        if (current_task->id != 1) return -NOS_EACCES;
        if (!timer_switches) kernel_fail("no hardware timer context switch observed");
        kernel_print("PASS timer_preemption\n");
        kernel_print(a ? "NOS TEST FAIL\n" : "NOS TEST PASS\n");
        arch_shutdown((int)a);
#endif
    default: return -NOS_ENOSYS;
    }
}
/* Another CPU may have faulted while a serialized mapping transaction had
 * temporarily invalidated its PTE. Recheck the committed mapping only for
 * architecture-reported translation/page faults, preserving all real faults. */
static int fault_mapping_resolved(void)
{
#if defined(NOS_MMU) && defined(NOS_LINUX_COMPAT)
    unsigned access = arch_fault_access();
    if (access != NOS_PROT_READ && access != NOS_PROT_WRITE && access != NOS_PROT_EXEC) return 0;
    uintptr_t address = arch_fault_address();
    if (!(arch_mapping_flags(current_task->space_id, address) & access)) return 0;
    return access == NOS_PROT_EXEC ? user_exec_access(current_task, address) :
        user_access(current_task, address, 1, access == NOS_PROT_WRITE);
#else
    return 0;
#endif
}
#ifdef NOS_ESP_IDF
/* The shared-memory MCU profile enters C directly, using SDK context switching
 * to preserve registers/TLS. Capabilities and user copies still pass dispatch. */
intptr_t nos_idf_syscall(uintptr_t number, uintptr_t a, uintptr_t b, uintptr_t c)
{
    if (!current_task || !arch_syscall_ready()) return -NOS_EDEADLK;
    struct task *caller = current_task;
    uintptr_t irq = spin_lock_irqsave(&kernel_lock);
    reschedule = 0;
    caller->frame.result = dispatch(number, a, b, c);
    unsigned yield = reschedule || caller->state != TASK_READY;
    spin_unlock_irqrestore(&kernel_lock, irq);
    if (yield) {
        extern void vPortYield(void);
        vPortYield();
    }
    return (intptr_t)caller->frame.result;
}
#ifdef NOS_XTENSA
intptr_t nos_xtensa_syscall(uintptr_t number, uintptr_t a, uintptr_t b, uintptr_t c)
{
    return nos_idf_syscall(number, a, b, c);
}
#endif
#endif
struct arch_frame *kernel_trap(struct arch_frame *frame, unsigned event)
{
    if (!current_task) kernel_fail("trap before task initialization");
#if defined(NOS_ARM64)
    if (frame->status & 15) kernel_fail("privileged_exception");
#elif defined(NOS_RISCV)
    if (frame->status & (1u << 8)) kernel_fail("privileged_exception");
#elif defined(NOS_ARM)
    if ((frame->status & 31) != 0x10) kernel_fail("privileged_exception");
#endif
    uintptr_t irq = spin_lock_irqsave(&kernel_lock);
#ifdef NOS_ARM_M
    k_memcpy(&current_task->frame, frame, offsetof(struct arch_frame, sp));
    current_task->frame.sp = (uintptr_t)frame + offsetof(struct arch_frame, sp);
#else
    k_memcpy(&current_task->frame, frame, sizeof(*frame));
#endif
    reschedule = 0;
    unsigned hardware_irq = (event == ARCH_TIMER || event == ARCH_IRQ) ? arch_irq_claim() : 0;
    if (hardware_irq) {
        arch_irq_mask(hardware_irq, 1); arch_irq_complete(hardware_irq);
        kernel_irq_notify(hardware_irq);
    }
    if (current_task->cancel_pending) {
        current_task->cancel_pending = 0;
        if (event == ARCH_TIMER && !hardware_irq) { arch_timer_ack(); scheduler_tick(); }
        task_finish(current_task, -NOS_EINTR);
    } else if (event == ARCH_TIMER || event == ARCH_IRQ) {
        if (event == ARCH_TIMER && !hardware_irq) { arch_timer_ack(); scheduler_tick(); }
        reschedule = 1;
    } else if (event == ARCH_FAULT) {
        if (!fault_mapping_resolved()) {
#if defined(NOS_MMU) && defined(NOS_LINUX_COMPAT)
            if (current_task->abi == NOS_ABI_LINUX) {
                if (linux_signal_send(current_task, arch_fault_signal(), 1, 0, arch_fault_address()))
                    task_finish(current_task, -NOS_EFAULT);
            } else
#endif
            {
                if (current_task->id % NOS_TASKS == 1) kernel_fail("init task faulted");
                task_finish(current_task, -NOS_EFAULT);
            }
        }
    } else {
        uintptr_t nr, a, b, c;
#if defined(NOS_ESP_IDF)
        nr = frame->number; a = frame->r[0]; b = frame->r[1]; c = frame->r[2];
#elif defined(NOS_ARM64)
        nr = frame->x[8]; a = frame->x[0]; b = frame->x[1]; c = frame->x[2];
#elif defined(NOS_RISCV)
        nr = frame->x[17]; a = frame->x[10]; b = frame->x[11]; c = frame->x[12];
        current_task->frame.pc += 4;
#elif defined(NOS_ARM_M)
        nr = frame->ip; a = frame->r[0]; b = frame->r[1]; c = frame->r[2];
#else
        nr = frame->r[7]; a = frame->r[0]; b = frame->r[1]; c = frame->r[2];
#endif
#if defined(NOS_MMU) && defined(NOS_LINUX_COMPAT)
        if (current_task->abi == NOS_ABI_LINUX) {
            current_task->linux_syscall_frame = current_task->frame;
            current_task->linux_syscall_number = nr; current_task->linux_restart_pending = 0;
            uintptr_t arguments[6] = { a, b, c, 0, 0, 0 };
#if defined(NOS_ARM64)
            arguments[3] = frame->x[3]; arguments[4] = frame->x[4]; arguments[5] = frame->x[5];
#elif defined(NOS_RISCV)
            arguments[3] = frame->x[13]; arguments[4] = frame->x[14]; arguments[5] = frame->x[15];
#else
            arguments[3] = frame->r[3]; arguments[4] = frame->r[4]; arguments[5] = frame->r[5];
#endif
            FRAME_RESULT(&current_task->frame) = (uintptr_t)linux_dispatch(nr, arguments);
        } else
#endif
            FRAME_RESULT(&current_task->frame) = (uintptr_t)dispatch(nr, a, b, c);
    }
    /* A signal can be queued while this task runs userspace on another CPU.
     * Recheck after dispatch publishes an interruptible wait, before releasing
     * kernel_lock or allowing the task to sleep. Nonblocking results stand. */
    linux_signal_interrupt_pending(current_task);
    struct task *previous = current_task;
    previous->run_cpu = UINT32_MAX;
    struct task *next = current_task;
    if (reschedule || next->state != TASK_READY) next = select_task();
    if (event == ARCH_TIMER && !hardware_irq && next != current_task) timer_switches++;
    while (!next) {
        spin_unlock_irqrestore(&kernel_lock, irq);
        irq = spin_lock_irqsave(&kernel_lock); next = select_task();
    }
    for (;;) {
        current_task = next; next->run_cpu = arch_cpu_id();
        linux_signal_deliver(next);
        if (next->state == TASK_READY
#ifdef NOS_MMU
            && !next->linux_stopped
#endif
        ) break;
        next->run_cpu = UINT32_MAX; next = select_task();
        if (!next) kernel_fail("no runnable task after signal delivery");
    }
    arch_space_switch(next->space_id);
    if (next->tls_slot && task_copy_out(next, next->tls_slot, &next->tls_value, sizeof(uintptr_t)))
        kernel_fail("invalid TLS binding");
    struct arch_frame *result = arch_return_frame(&next->frame);
    spin_unlock_irqrestore(&kernel_lock, irq); return result;
}
void kernel_start(void)
{
    arch_platform_init();
    kernel_print("NOS microkernel " ARCH_NAME "\n");
    page_init();
#if defined(NOS_SELFTEST) && defined(NOS_CLOCK_TEST_EPOCH)
    kernel_ticks = (uint64_t)NOS_CLOCK_TEST_EPOCH;
#endif
    unsigned free = page_available();
    uintptr_t page = page_alloc(2);
    if (!page || page_free(page, 1) != -NOS_EINVAL || page_free(page, 2) ||
        page_free(page, 2) != -NOS_EINVAL || page_available() != free) kernel_fail("page allocator");
    page = page_alloc(PAGE_POOL_PAGES);
    if (!page || page_alloc(1) || page_free(page, PAGE_POOL_PAGES) || page_available() != free)
        kernel_fail("physical allocation exhaustion");
    kernel_print("PASS page_allocator\n");
    unsigned caps = NOS_CAP_USER | NOS_CAP_PREEMPT | NOS_CAP_IPC;
#ifdef NOS_MMU
    caps |= NOS_CAP_MMU;
#endif
#ifdef NOS_MPU
    caps |= NOS_CAP_MPU;
#endif
#if NOS_SMP > 1
    caps |= NOS_CAP_SMP;
#endif
#ifdef NOS_ESP_IDF
    extern void nos_esp_idf_boot(void);
    nos_esp_idf_boot();
#elif defined(NOS_NK60_V2)
    extern void nos_keyboard_service(uintptr_t unused);
    if (create_task(0, (uintptr_t)nos_keyboard_service, 0, 4,
        device_service_caps(caps), UINT32_MAX) < 0)
        kernel_fail("keyboard address space");
#else
    if (create_task(0, (uintptr_t)nos_vfs_server, 0, 4, device_service_caps(caps), UINT32_MAX) < 0 ||
        create_task(1,
#ifdef NOS_NORMAL_BOOT
        (uintptr_t)nos_init,
#else
        (uintptr_t)nos_validation,
#endif
        0, 8, caps | NOS_CAP_SPAWN, UINT32_MAX) < 0)
        kernel_fail("initial address spaces");
#endif
    extern void nos_idle(uintptr_t unused);
    for (unsigned cpu = 0; cpu < NOS_SMP; cpu++) {
        unsigned slot = NOS_IDLE_FIRST + cpu;
        if (create_task(slot, (uintptr_t)nos_idle, 0, 31, 0, UINT32_MAX) < 0) kernel_fail("idle address space");
        tasks[slot].pin_cpu = cpu;
    }
    ipc_init();
#ifdef NOS_ESP_IDF
    extern void nos_idf_flush_pending(void);
    nos_idf_flush_pending();
#endif
#if defined(NOS_SELFTEST) && defined(NOS_LINUX_COMPAT)
    if (!ipc_capacity_selftest()) kernel_fail("IPC high task slot regression");
    kernel_print("PASS ipc_capacity_high_slots\n");
#endif
#ifdef NOS_SELFTEST
    if (!memory_transaction_selftest()) kernel_fail("memory mapping transaction rollback");
    kernel_print("PASS mapping_transaction_rollback\n");
#endif
    current_task = select_task(); current_task->run_cpu = arch_cpu_id();
    arch_space_switch(current_task->space_id);
    arch_start_secondary();
    arch_timer_init();
    arch_enter_user(arch_return_frame(&current_task->frame));
}

void kernel_secondary_start(void)
{
    arch_secondary_init();
    uintptr_t irq = spin_lock_irqsave(&kernel_lock);
    struct task *next = select_task();
    while (!next) {
        spin_unlock_irqrestore(&kernel_lock, irq);
        irq = spin_lock_irqsave(&kernel_lock); next = select_task();
    }
    current_task = next; next->run_cpu = arch_cpu_id();
    arch_space_switch(next->space_id); arch_timer_init();
    struct arch_frame *frame = arch_return_frame(&next->frame);
    spin_unlock_irqrestore(&kernel_lock, irq); arch_enter_user(frame);
}
