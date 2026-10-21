/* Copyright (C) 2026 NOS contributors */
#include "kernel.h"
#include "elf.h"
#if defined(NOS_MMU) && defined(NOS_LINUX_COMPAT)
#ifdef NOS_ARM
#define LNR(arm, generic) (arm)
#else
#define LNR(arm, generic) (generic)
#endif
#define CLONE_VM 0x100u
#define CLONE_THREAD 0x10000u
#define CLONE_SETTLS 0x80000u
#define CLONE_PARENT_SETTID 0x100000u
#define CLONE_CHILD_CLEARTID 0x200000u
#define CLONE_CHILD_SETTID 0x1000000u
static struct task exec_image;
static unsigned exec_owner, exec_pending;
unsigned linux_process_group(const struct task *task)
{
    struct task *leader = find_task(task->space_owner);
    return leader && leader->linux_pgid ? leader->linux_pgid : task->linux_pgid ? task->linux_pgid : 1;
}
unsigned linux_process_session(const struct task *task)
{
    struct task *leader = find_task(task->space_owner);
    return leader && leader->linux_sid ? leader->linux_sid : task->linux_sid ? task->linux_sid : 1;
}
static void frame_set_sp(struct arch_frame *frame, uintptr_t sp)
{
#ifdef NOS_RISCV
    frame->x[2] = sp;
#else
    frame->sp = sp;
#endif
}
static void frame_set_arg(struct arch_frame *frame, uintptr_t arg)
{
#ifdef NOS_ARM64
    frame->x[0] = arg;
#elif defined(NOS_RISCV)
    frame->x[10] = arg;
#else
    frame->r[0] = arg;
#endif
}
static void frame_set_tls(struct arch_frame *frame, uintptr_t tls)
{
#ifdef NOS_RISCV
    frame->x[4] = tls;
#else
    frame->tls = tls;
#endif
}
int linux_task_prepare(struct task *task, uintptr_t entry)
{
    if (entry < NOS_LINUX_GATEWAY_BASE || entry >= NOS_LINUX_GATEWAY_END || !user_exec_access(task, entry))
        return -NOS_EINVAL;
    uintptr_t base = NOS_LINUX_GATEWAY_STACK_BASE + (task->id % NOS_TASKS) * NOS_LINUX_GATEWAY_STACK_STRIDE + NOS_PAGE_SIZE;
    int rc = vm_gateway_map(task, base, NOS_LINUX_GATEWAY_STACK_BYTES, NOS_PROT_READ | NOS_PROT_WRITE);
    if (rc) return rc;
    task->abi = NOS_ABI_LINUX; task->linux_gateway_entry = entry;
    task->linux_gateway_stack = base + NOS_LINUX_GATEWAY_STACK_BYTES;
    task->linux_gateway_active = 0;
    linux_signal_reset(task);
    return 0;
}
int linux_gateway_clone(struct task *child, const struct task *parent)
{
    return linux_task_prepare(child, parent->linux_gateway_entry);
}
static int exec_commit(struct task *task)
{
    unsigned pgid = linux_process_group(task), sid = linux_process_session(task);
    unsigned old_space = task->space_id;
    int rc = space_commit_exec(task, &exec_image); if (rc) return rc;
    credentials_exec(task);
    linux_signal_exec(task, old_space);
    task->frame = exec_image.frame; task->abi = NOS_ABI_LINUX;
    task->linux_gateway_entry = exec_image.linux_gateway_entry;
    task->linux_gateway_stack = exec_image.linux_gateway_stack;
    task->linux_gateway_active = task->linux_waitcontrol = task->linux_wait = 0;
    task->clear_tid = task->tls_slot = task->tls_value = task->linux_robust_list = 0;
    task->linux_sleep_remain = 0; task->state = TASK_READY;
    task->linux_pgid = pgid; task->linux_sid = sid; task->linux_execed = 1;
    if (task->id != task->space_owner) {
        struct task *leader = find_task(task->space_owner);
        if (!leader || leader->state != TASK_DEAD) kernel_fail("exec missing process leader");
#ifdef NOS_LINUX_COMPAT
        if (!task->credential_ref || leader->credential_ref != task->credential_ref)
            kernel_fail("exec credential ownership");
#endif
        unsigned generation = leader->generation, parent = leader->parent, process = task->space_owner;
        linux_signal_retid(task, leader);
        k_memcpy(leader, task, sizeof(*leader));
        leader->id = process; leader->generation = generation; leader->parent = parent; leader->is_thread = 0;
        for (unsigned cpu = 0; cpu < NOS_CPUS; cpu++) if (cpu_tasks[cpu] == task) cpu_tasks[cpu] = leader;
        task->memory = 0; task_reap(task);
    }
    exec_pending = 0; return 0;
}
void linux_exec_progress(void)
{
    if (!exec_pending) return;
    struct task *task = find_task(exec_owner);
    if (!task || task->state == TASK_DEAD) { space_destroy(&exec_image); exec_pending = 0; return; }
    for (unsigned i = 2; i < NOS_IDLE_FIRST; i++) {
        struct task *peer = &tasks[i];
        if (peer != task && peer->space_owner == task->space_owner && peer->state != TASK_UNUSED &&
            peer->state != TASK_DEAD && peer->state != TASK_RETIRED) return;
    }
    uint64_t limits[2];
    process_as_limits(task, limits);
    int rc = vm_as_usage(&exec_image) > limits[0] ? -NOS_ENOMEM : 0;
    if (!rc) {
        vm_as_limit_set(&exec_image, limits[0]);
        rc = ipc_process_notice(NOS_IPC_PROCESS_EXEC, task->space_owner, 0);
    }
    if (rc == -NOS_EAGAIN) return;
    if (!rc) rc = exec_commit(task);
    if (rc) {
        space_destroy(&exec_image); exec_pending = 0; task->state = TASK_READY;
        FRAME_RESULT(&task->frame) = (uintptr_t)rc;
    }
    reschedule = 1;
}
intptr_t linux_exec_current(uintptr_t pointer, uintptr_t entropy, uintptr_t flags)
{
    struct task *task = current_task;
    if (task->abi != NOS_ABI_GATEWAY || !task->linux_gateway_active) return -NOS_EPERM;
    if (flags != NOS_EXEC_RANDOM_VALID || !entropy) return -NOS_EAGAIN;
    if (exec_pending) return -NOS_EAGAIN;
    struct nos_exec_request request; static struct nos_exec_args args;
    if (user_copy_in(&request, pointer, sizeof(request))) return -NOS_EFAULT;
    int rc = nos_exec_copy_args(&request, &args); if (rc) return rc;
    if (user_copy_in(args.random, entropy, sizeof(args.random))) return -NOS_EFAULT;
    args.random_valid = 1;
    size_t bytes; const void *image = nos_initramfs_find(args.path, &bytes); if (!image) return -NOS_ENOENT;
    k_memset(&exec_image, 0, sizeof(exec_image)); exec_image.id = task->space_owner;
    rc = space_create_empty(&exec_image); if (rc) return rc;
    uint64_t limits[2];
    process_as_limits(task, limits);
    vm_as_limit_set(&exec_image, limits[0]);
    struct nos_elf_result result;
    rc = nos_elf_load_linux(&exec_image, image, bytes, &args, credentials_get(task), &result);
    if (!rc) {
        uintptr_t base = NOS_LINUX_GATEWAY_STACK_BASE + (task->space_owner % NOS_TASKS) * NOS_LINUX_GATEWAY_STACK_STRIDE + NOS_PAGE_SIZE;
        rc = vm_gateway_map(&exec_image, base, NOS_LINUX_GATEWAY_STACK_BYTES, NOS_PROT_READ | NOS_PROT_WRITE);
        exec_image.linux_gateway_stack = base + NOS_LINUX_GATEWAY_STACK_BYTES;
        exec_image.linux_gateway_entry = result.gateway_entry;
        arch_frame_init(&exec_image.frame, result.entry, result.stack, result.argc, result.argv);
    }
    if (rc) { space_destroy(&exec_image); return rc; }
    exec_owner = task->id; exec_pending = 1;
    for (unsigned i = 2; i < NOS_IDLE_FIRST; i++) {
        struct task *peer = &tasks[i];
        if (peer == task || peer->space_owner != task->space_owner || peer->state == TASK_UNUSED ||
            peer->state == TASK_DEAD || peer->state == TASK_RETIRED) continue;
        if (peer->run_cpu != UINT32_MAX && peer->run_cpu != arch_cpu_id()) peer->cancel_pending = 1;
        else task_finish(peer, 0);
    }
    task->state = TASK_EXEC; linux_exec_progress(); reschedule = 1; return 0;
}
static intptr_t gateway_enter(uintptr_t number, const uintptr_t arguments[6])
{
    if (!current_task->linux_gateway_entry || current_task->linux_gateway_active) return -NOS_ENOSYS;
    struct nos_linux_call call; call.number = number;
    for (unsigned i = 0; i < 6; i++) call.args[i] = arguments[i];
    uintptr_t request = (current_task->linux_gateway_stack - sizeof(call)) & ~(uintptr_t)15;
    if (user_copy_out(request, &call, sizeof(call))) return -NOS_EFAULT;
    k_memcpy(&current_task->linux_frame, &current_task->frame, sizeof(current_task->frame));
    current_task->linux_gateway_active = 1; current_task->abi = NOS_ABI_GATEWAY;
    current_task->frame.pc = current_task->linux_gateway_entry;
#ifdef NOS_ARM
    if (current_task->frame.pc & 1) current_task->frame.status |= 1u << 5;
    else current_task->frame.status &= ~(1u << 5);
    current_task->frame.pc &= ~(uintptr_t)1;
#endif
    frame_set_sp(&current_task->frame, request); frame_set_arg(&current_task->frame, request);
    return (intptr_t)request;
}
intptr_t linux_gateway_return(uintptr_t result)
{
    if (current_task->abi != NOS_ABI_GATEWAY || !current_task->linux_gateway_active) return -NOS_EPERM;
    linux_wait_control_end(current_task);
    k_memcpy(&current_task->frame, &current_task->linux_frame, sizeof(current_task->frame));
    current_task->abi = NOS_ABI_LINUX; current_task->linux_gateway_active = 0;
    if ((intptr_t)result == -NOS_EINTR) linux_signal_interrupted(current_task);
    if ((intptr_t)result == -NOS_EPIPE) {
#ifdef NOS_ARM
        uintptr_t number = current_task->frame.r[7];
        int write_call = number == 4 || number == 146 || number == 181;
#else
#ifdef NOS_ARM64
        uintptr_t number = current_task->frame.x[8];
#else
        uintptr_t number = current_task->frame.x[17];
#endif
        int write_call = number == 64 || number == 66 || number == 68;
#endif
        if (write_call) (void)linux_signal_send(current_task, 13, 0, current_task->space_owner, 0);
    }
    return (intptr_t)result;
}
static int copy_timespec(uintptr_t pointer, int wide, uint64_t *ticks)
{
    int64_t seconds, nanos;
    if (wide) {
        int64_t words[2]; if (user_copy_in(words, pointer, sizeof(words))) return -NOS_EFAULT;
        seconds = words[0]; nanos = words[1];
    } else {
        int32_t words[2]; if (user_copy_in(words, pointer, sizeof(words))) return -NOS_EFAULT;
        seconds = words[0]; nanos = words[1];
    }
    if (seconds < 0 || nanos < 0 || nanos >= 1000000000 || (uint64_t)seconds > (UINT64_MAX - 100) / 100) return -NOS_EINVAL;
    *ticks = (uint64_t)seconds * 100 + ((uint64_t)nanos + 9999999) / 10000000;
    return 0;
}
static int put_timespec(uintptr_t pointer, uint64_t ticks, int wide)
{
    if (wide) {
        int64_t words[2] = { (int64_t)(ticks / 100), (int64_t)(ticks % 100) * 10000000 };
        return user_copy_out(pointer, words, sizeof(words));
    }
    if (ticks / 100 > INT32_MAX) return -NOS_EOVERFLOW;
    int32_t words[2] = { (int32_t)(ticks / 100), (int32_t)(ticks % 100) * 10000000 };
    return user_copy_out(pointer, words, sizeof(words));
}
static int clock_snapshot_ns(uintptr_t clock, uint64_t *nanoseconds)
{
    struct nos_clock_value value; int rc = kernel_clock_snapshot(clock, &value); if (rc) return rc;
    if ((uint64_t)value.seconds > (UINT64_MAX - (uint64_t)value.nanoseconds) / 1000000000) return -NOS_EOVERFLOW;
    *nanoseconds = (uint64_t)value.seconds * 1000000000 + (uint64_t)value.nanoseconds; return 0;
}
static int clock_snapshot(uintptr_t clock, uint64_t *ticks)
{ uint64_t ns; int rc = clock_snapshot_ns(clock, &ns); if (!rc) *ticks = ns / 10000000; return rc; }
static struct task *process_leader(void)
{ struct task *leader = find_task(current_task->space_owner); return leader ? leader : current_task; }
static int timer_read(uintptr_t pointer, uint64_t *interval, uint64_t *value)
{
    intptr_t time[4]; if (user_copy_in(time, pointer, sizeof(time))) return -NOS_EFAULT;
    uint64_t ticks[2];
    for (unsigned i = 0; i < 2; i++) {
        if (time[i * 2] < 0 || time[i * 2 + 1] < 0 || time[i * 2 + 1] >= 1000000 ||
            (uint64_t)time[i * 2] > (UINT64_MAX - kernel_ticks - 100) / 100) return -NOS_EINVAL;
        ticks[i] = (uint64_t)time[i * 2] * 100 + ((uint64_t)time[i * 2 + 1] + 9999) / 10000;
    }
    *interval = ticks[0]; *value = ticks[1]; return 0;
}
static int timer_write(uintptr_t pointer, const struct task *leader)
{
    uint64_t remain = leader->linux_alarm_deadline > kernel_ticks ? leader->linux_alarm_deadline - kernel_ticks : 0;
    uint64_t interval = leader->linux_alarm_interval;
    if (interval / 100 > INTPTR_MAX || remain / 100 > INTPTR_MAX) return -NOS_EOVERFLOW;
    intptr_t time[4] = { (intptr_t)(interval / 100), (intptr_t)(interval % 100) * 10000,
                         (intptr_t)(remain / 100), (intptr_t)(remain % 100) * 10000 };
    return user_copy_out(pointer, time, sizeof(time));
}
void linux_timer_tick(void)
{
    for (unsigned i = 2; i < NOS_IDLE_FIRST; i++) {
        struct task *leader = &tasks[i];
        if (leader->id != leader->space_owner || leader->abi == NOS_ABI_NATIVE || !leader->linux_alarm_deadline ||
            leader->state == TASK_UNUSED || leader->state == TASK_RETIRED || leader->linux_alarm_deadline > kernel_ticks) continue;
        if (leader->linux_alarm_interval) {
            uint64_t periods = (kernel_ticks - leader->linux_alarm_deadline) / leader->linux_alarm_interval + 1;
            if (periods > (UINT64_MAX - leader->linux_alarm_deadline) / leader->linux_alarm_interval) leader->linux_alarm_deadline = 0;
            else leader->linux_alarm_deadline += periods * leader->linux_alarm_interval;
        } else leader->linux_alarm_deadline = 0;
        struct task *target = NULL;
        for (unsigned n = 2; n < NOS_IDLE_FIRST; n++) {
            struct task *peer = &tasks[n];
            if (peer->space_owner != leader->id || peer->state == TASK_UNUSED || peer->state == TASK_DEAD || peer->state == TASK_RETIRED) continue;
            if (!target || !(peer->signal_mask & (1ull << 13))) target = peer;
            if (!(peer->signal_mask & (1ull << 13))) break;
        }
        if (target) (void)linux_signal_send(target, 14, 128, 0, 0);
    }
}
static intptr_t linux_sleep(uintptr_t clock, uintptr_t flags, uintptr_t request, uintptr_t remain, int wide)
{
    if ((clock != 0 && clock != 1) || flags > 1) return -NOS_EINVAL;
    uint64_t ticks; int rc = copy_timespec(request, wide, &ticks); if (rc) return rc;
    if (flags) {
        uint64_t now; rc = clock_snapshot(clock, &now); if (rc) return rc;
        if (ticks <= now) return 0;
        ticks -= now;
    }
    if (UINT64_MAX - kernel_ticks < ticks) return -NOS_EINVAL;
    if (remain && !flags) { rc = put_timespec(remain, 0, wide); if (rc) return rc; }
    current_task->linux_sleep_remain = flags ? 0 : remain; current_task->linux_sleep_wide = (unsigned)wide;
    if (ticks) { current_task->wake_tick = kernel_ticks + ticks; current_task->state = TASK_SLEEP; reschedule = 1; }
    return 0;
}
static intptr_t linux_futex(const uintptr_t a[6], int wide)
{
    unsigned operation = (unsigned)a[1] & ~(128u | 256u);
    unsigned shared = !(a[1] & 128u);
    if ((a[1] & 256u) && operation != 9) return -NOS_ENOSYS;
    if (operation == 1) return futex_wake_mode(a[0], a[2], shared);
    if (operation == 3 || operation == 4) {
        struct nos_futex_requeue req = { a[0], a[4], (uint32_t)a[2], (uint32_t)a[3], (uint32_t)a[5], operation == 4 };
        /* Reuse the native validated request path using a kernel-owned helper. */
        return futex_requeue_mode(&req, shared);
    }
    if (operation != 0 && operation != 9) return -NOS_ENOSYS;
    uint64_t ticks = 0;
    if (a[3]) {
        int rc = copy_timespec(a[3], wide, &ticks); if (rc) return rc;
        if (operation == 9) {
            uint64_t now; int result = clock_snapshot(a[1] & 256u ? 0 : 1, &now); if (result) return result;
            if (ticks <= now) return -NOS_ETIMEDOUT;
            ticks -= now;
        }
        if (!ticks) return -NOS_ETIMEDOUT;
    }
    if (operation == 9 && (uint32_t)a[5] != UINT32_MAX) return -NOS_ENOTSUP;
    if (ticks > UINTPTR_MAX) return -NOS_EINVAL;
    return futex_wait_mode(a[0], (uint32_t)a[2], (uintptr_t)ticks, shared);
}
static intptr_t linux_fork(const uintptr_t arguments[6])
{
    uintptr_t flags = arguments[0];
    if ((flags & CLONE_PARENT_SETTID) && !user_access(current_task, arguments[2], 4, 1)) return -NOS_EFAULT;
    if ((flags & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)) && !user_access(current_task, arguments[4], 4, 1)) return -NOS_EFAULT;
    for (unsigned i = 2; i < NOS_IDLE_FIRST; i++) {
        struct task *child = &tasks[i]; if (child->state != TASK_UNUSED) continue;
        if (child->credential_ref) kernel_fail("fork with credentials");
        unsigned generation = child->generation; k_memset(child, 0, sizeof(*child));
        child->generation = generation; child->id = generation * NOS_TASKS + i;
        child->parent = current_task->space_owner;
        child->priority = child->base_priority = current_task->base_priority;
        child->caps = current_task->caps; child->run_cpu = child->pin_cpu = UINT32_MAX;
        child->linux_pgid = linux_process_group(current_task); child->linux_sid = linux_process_session(current_task);
        int rc = space_fork(child, current_task); if (rc) return rc;
        rc = credentials_create(child, current_task);
        if (rc) { space_destroy(child); return rc; }
        child->frame = current_task->frame; FRAME_RESULT(&child->frame) = 0;
        rc = linux_gateway_clone(child, current_task);
        /* Fork duplicates existing mappings even if the parent lowered its
         * limit below current usage. Only subsequent growth is limited. */
        uint64_t limits[2];
        process_as_limits(child, limits);
        vm_as_limit_set(child, limits[0]);
        unsigned tid = child->id;
        if (!rc && (flags & CLONE_CHILD_SETTID)) rc = task_copy_out(child, arguments[4], &tid, sizeof(tid));
        if (!rc && (flags & CLONE_PARENT_SETTID)) rc = task_copy_out(current_task, arguments[2], &tid, sizeof(tid));
        if (!rc) rc = ipc_process_notice(NOS_IPC_PROCESS_FORK, current_task->space_owner, child->space_owner);
        if (rc) { space_destroy(child); credentials_release(child); return rc; }
        linux_signal_fork(child, current_task);
        if (flags & CLONE_CHILD_CLEARTID) child->clear_tid = arguments[4];
        child->mutex_caps = current_task->mutex_caps; ipc_task_inherit(child, current_task);
        child->state = TASK_READY; reschedule = 1; return child->id;
    }
    return -NOS_EAGAIN;
}
static int linux_wait_status(const struct task *child)
{ return child->linux_exit_signal ? (int)(child->linux_exit_signal & 127u) : (int)((child->exit_status & 255) << 8); }
int linux_wait_wake(struct task *waiter, struct task *child)
{
    if (!waiter->linux_wait || child->parent != waiter->space_owner ||
        (waiter->wait_for != UINT32_MAX && waiter->wait_for != child->id)) return 0;
    int status = linux_wait_status(child);
    int rc = waiter->pending ? task_copy_out(waiter, waiter->pending, &status, sizeof(status)) : 0;
    FRAME_RESULT(&waiter->frame) = (uintptr_t)(rc ? rc : (intptr_t)child->id);
    waiter->linux_wait = 0; waiter->state = TASK_READY;
    /* A failed status copy leaves the zombie available to a subsequent wait. */
    return rc ? -1 : 1;
}
static intptr_t linux_wait4(const uintptr_t a[6])
{
    intptr_t wanted = (intptr_t)a[0];
    if (wanted == 0 || wanted < -1) return -NOS_ENOTSUP;
    if (a[2] & ~(uintptr_t)1) return -NOS_ENOTSUP;
    if (a[3]) return -NOS_ENOTSUP;
    if (a[1] && !user_access(current_task, a[1], sizeof(int), 1)) return -NOS_EFAULT;
    unsigned found = 0;
    for (unsigned i = 2; i < NOS_IDLE_FIRST; i++) {
        struct task *child = &tasks[i];
        if (child->state == TASK_UNUSED || child->state == TASK_RETIRED || child->is_thread ||
            child->parent != current_task->space_owner || (wanted != -1 && child->id != (uintptr_t)wanted)) continue;
        found = 1; if (!task_is_waitable(child)) continue;
        int status = linux_wait_status(child);
        int rc = a[1] ? user_copy_out(a[1], &status, sizeof(status)) : 0;
        if (rc) return rc;
        intptr_t pid = child->id; task_reap(child); return pid;
    }
    if (!found) return -NOS_ECHILD;
    if (a[2] & 1) return 0;
    current_task->linux_wait = 1; current_task->linux_wait_options = (unsigned)a[2];
    current_task->wait_for = wanted == -1 ? UINT32_MAX : (unsigned)wanted;
    current_task->pending = a[1]; current_task->state = TASK_WAIT; reschedule = 1; return 0;
}
static intptr_t linux_clone(const uintptr_t a[6])
{
    uintptr_t flags = a[0], stack = a[1];
    if (!(flags & CLONE_VM) && (flags & 255u) == 17 &&
        !(flags & ~(uintptr_t)(255u | CLONE_PARENT_SETTID | CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)) && !stack)
        return linux_fork(a);
    const uintptr_t supported = CLONE_VM | 0x200u | 0x400u | 0x800u | CLONE_THREAD | 0x40000u |
        CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID | 0x400000u;
    if ((flags & ~supported) || !(flags & CLONE_VM) || !(flags & CLONE_THREAD)) return -NOS_ENOSYS;
    if (!stack || (stack & (sizeof(uintptr_t) == 4 ? 7u : 15u)) || !user_access(current_task, stack - 64, 64, 1)) return -NOS_EFAULT;
    if ((flags & CLONE_PARENT_SETTID) && !user_access(current_task, a[2], 4, 1)) return -NOS_EFAULT;
    if ((flags & (CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID)) && !user_access(current_task, a[4], 4, 1)) return -NOS_EFAULT;
    for (unsigned i = 2; i < NOS_IDLE_FIRST; i++) {
        struct task *child = &tasks[i]; if (child->state != TASK_UNUSED) continue;
        if (child->credential_ref) kernel_fail("clone with credentials");
        unsigned generation = child->generation; k_memset(child, 0, sizeof(*child));
        child->generation = generation; child->id = generation * NOS_TASKS + i;
        child->parent = current_task->id; child->is_thread = 1;
        child->priority = child->base_priority = current_task->base_priority;
        child->caps = current_task->caps & ~NOS_CAP_SPAWN;
        child->linux_pgid = linux_process_group(current_task); child->linux_sid = linux_process_session(current_task);
        child->run_cpu = child->pin_cpu = UINT32_MAX;
        int rc = space_clone(child, current_task, stack); if (rc) return rc;
        rc = credentials_share(child, current_task);
        if (rc) { space_destroy(child); return rc; }
        k_memcpy(&child->frame, &current_task->frame, sizeof(child->frame)); frame_set_sp(&child->frame, stack);
        FRAME_RESULT(&child->frame) = 0;
        if (flags & CLONE_SETTLS) frame_set_tls(&child->frame, a[3]);
        rc = linux_gateway_clone(child, current_task);
        if (rc) { space_destroy(child); credentials_release(child); return rc; }
        child->signal_mask = current_task->signal_mask;
        if (flags & CLONE_CHILD_CLEARTID) child->clear_tid = a[4];
        unsigned tid = child->id;
        if (((flags & CLONE_PARENT_SETTID) && task_copy_out(current_task, a[2], &tid, 4)) ||
            ((flags & CLONE_CHILD_SETTID) && task_copy_out(child, a[4], &tid, 4))) {
            space_destroy(child); credentials_release(child); return -NOS_EFAULT;
        }
        child->mutex_caps = current_task->mutex_caps; ipc_task_inherit(child, current_task);
        child->state = TASK_READY; reschedule = 1; return child->id;
    }
    return -NOS_EAGAIN;
}
static struct task *linux_find_process(uintptr_t pid)
{
    struct task *task = find_task(pid);
    return task && task->abi != NOS_ABI_NATIVE && task->space_owner == task->id ? task : NULL;
}
static intptr_t linux_setpgid(uintptr_t pid, uintptr_t group)
{
    if (!pid) pid = current_task->space_owner;
    struct task *target = linux_find_process(pid);
    if (!target || !group) { if (!group) group = pid; if (!target) return -NOS_ESRCH; }
    if (group > INT32_MAX) return -NOS_EINVAL;
    if (target->id != current_task->space_owner && target->parent != current_task->space_owner) return -NOS_ESRCH;
    if (target->id != current_task->space_owner && target->linux_execed) return -NOS_EACCES;
    unsigned session = linux_process_session(current_task);
    if (linux_process_session(target) != session || target->id == session) return -NOS_EPERM;
    if (group != target->id) {
        unsigned found = 0;
        for (unsigned i = 2; i < NOS_IDLE_FIRST; i++) if (tasks[i].state != TASK_UNUSED && tasks[i].state != TASK_RETIRED &&
            tasks[i].id == tasks[i].space_owner && linux_process_group(&tasks[i]) == group && linux_process_session(&tasks[i]) == session) found = 1;
        if (!found) return -NOS_EPERM;
    }
    for (unsigned i = 2; i < NOS_IDLE_FIRST; i++) if (tasks[i].space_owner == target->id &&
        tasks[i].state != TASK_UNUSED && tasks[i].state != TASK_RETIRED) tasks[i].linux_pgid = (unsigned)group;
    return 0;
}
static intptr_t linux_setsid(void)
{
    unsigned pid = current_task->space_owner;
    for (unsigned i = 2; i < NOS_IDLE_FIRST; i++) if (tasks[i].state != TASK_UNUSED && tasks[i].state != TASK_RETIRED &&
        tasks[i].id == tasks[i].space_owner && tasks[i].abi != NOS_ABI_NATIVE && linux_process_group(&tasks[i]) == pid) return -NOS_EPERM;
    for (unsigned i = 2; i < NOS_IDLE_FIRST; i++) if (tasks[i].space_owner == pid && tasks[i].state != TASK_UNUSED && tasks[i].state != TASK_RETIRED)
        tasks[i].linux_pgid = tasks[i].linux_sid = pid;
    return pid;
}
static void robust_release(struct task *task, uintptr_t node, intptr_t offset)
{
    uintptr_t address;
    if (offset < 0) {
        uintptr_t amount = (uintptr_t)(-(offset + 1)) + 1;
        if (node < amount) return;
        address = node - amount;
    } else {
        if (node > UINTPTR_MAX - (uintptr_t)offset) return;
        address = node + (uintptr_t)offset;
    }
    if ((address & 3) || !user_access(task, address, 4, 1)) return;
    uintptr_t physical = task_user_phys(task, address); if (!physical) return;
    unsigned *word = (unsigned *)physical, value = __atomic_load_n(word, __ATOMIC_ACQUIRE);
    while ((value & 0x3fffffffu) == task->id) {
        unsigned desired = (value & 0x80000000u) | 0x40000000u;
        if (__atomic_compare_exchange_n(word, &value, desired, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            futex_wake_task(task, address, 1); break;
        }
    }
}
void linux_task_exit(struct task *task)
{
    if (exec_pending && task->id == exec_owner) { space_destroy(&exec_image); exec_pending = 0; }
    linux_signal_discard(task);
    if (task->linux_robust_list) {
        uintptr_t head[3];
        if (!task_copy_in(task, head, task->linux_robust_list, sizeof(head))) {
            uintptr_t node = head[0];
            for (unsigned visited = 0; visited < 2048 && node && node != task->linux_robust_list; visited++) {
                uintptr_t next;
                if (task_copy_in(task, &next, node, sizeof(next))) break;
                if (node != head[2]) robust_release(task, node, (intptr_t)head[1]);
                if (next == node) break;
                node = next;
            }
            if (head[2]) robust_release(task, head[2], (intptr_t)head[1]);
        }
        task->linux_robust_list = 0;
    }
    if (task->linux_gateway_stack) {
        (void)vm_gateway_unmap(task, task->linux_gateway_stack - NOS_LINUX_GATEWAY_STACK_BYTES,
                               NOS_LINUX_GATEWAY_STACK_BYTES);
        task->linux_gateway_stack = 0;
    }
}
intptr_t linux_dispatch(uintptr_t number, const uintptr_t a[6])
{
    unsigned credentials_handled;
    intptr_t credentials_result = linux_credentials_syscall(number, a, &credentials_handled);
    if (credentials_handled) return credentials_result;
    intptr_t signal_result = linux_signal_syscall(number, a);
    if (signal_result != NOS_LINUX_NOT_HANDLED) return signal_result;
#ifdef NOS_ARM
    const int wide = number == 403 || number == 406 || number == 407 || number == 422;
#else
    const int wide = 1;
#endif
    switch (number) {
#ifdef NOS_ARM
    case 2: { const uintptr_t args[6] = {17, 0, 0, 0, 0, 0}; return linux_fork(args); }
#endif
    case LNR(114, 260): return linux_wait4(a);
    case LNR(1, 93): task_finish(current_task, (intptr_t)a[0]); return 0;
    case LNR(248, 94): task_exit_group(current_task, (intptr_t)a[0]); return 0;
    case LNR(345, 168): {
        unsigned cpu = arch_cpu_id(), node = 0;
        if (a[0] && user_copy_out(a[0], &cpu, sizeof(cpu))) return -NOS_EFAULT;
        if (a[1] && user_copy_out(a[1], &node, sizeof(node))) return -NOS_EFAULT;
        return 0;
    }
    case LNR(20, 172): return current_task->space_owner;
    case LNR(57, 154): return linux_setpgid(a[0], a[1]);
    case LNR(132, 155): { struct task *target = a[0] ? linux_find_process(a[0]) : current_task;
        return target ? (intptr_t)linux_process_group(target) : -NOS_ESRCH; }
    case LNR(147, 156): { struct task *target = a[0] ? linux_find_process(a[0]) : current_task;
        return target ? (intptr_t)linux_process_session(target) : -NOS_ESRCH; }
    case LNR(66, 157): return linux_setsid();
#ifdef NOS_ARM
    case 65: return linux_process_group(current_task);
#endif
    case LNR(224, 178): return current_task->id;
    case LNR(64, 173): { struct task *p = find_task(process_leader()->parent); return p ? p->space_owner : 0; }
    case LNR(78, 169): {
        struct nos_clock_value value; int rc = kernel_clock_snapshot(0, &value); if (rc) return rc;
        if (value.seconds > INTPTR_MAX) return -NOS_EOVERFLOW;
        intptr_t time[2] = { (intptr_t)value.seconds, (intptr_t)(value.nanoseconds / 1000) };
        if (a[0] && user_copy_out(a[0], time, sizeof(time))) return -NOS_EFAULT;
        if (a[1]) { int timezone[2] = {0, 0}; if (user_copy_out(a[1], timezone, sizeof(timezone))) return -NOS_EFAULT; }
        return 0;
    }
#ifdef NOS_ARM
    case 13: {
        struct nos_clock_value value; int rc = kernel_clock_snapshot(0, &value); if (rc) return rc;
        if (value.seconds > INT32_MAX) return -NOS_EOVERFLOW;
        int32_t seconds = (int32_t)value.seconds;
        if (a[0] && user_copy_out(a[0], &seconds, sizeof(seconds))) return -NOS_EFAULT;
        return seconds;
    }
#endif
    case LNR(122, 160): {
        char identity[6][65]; k_memset(identity, 0, sizeof(identity));
        k_memcpy(identity[0], "NOS", 4); k_memcpy(identity[1], "nos", 4);
        k_memcpy(identity[2], "microkernel", 12); k_memcpy(identity[3], "NOS ABI 1", 10);
#ifdef NOS_ARM64
        k_memcpy(identity[4], "aarch64", 8);
#elif defined(NOS_RISCV)
        k_memcpy(identity[4], "riscv64", 8);
#else
        k_memcpy(identity[4], "armv7l", 7);
#endif
        return user_copy_out(a[0], identity, sizeof(identity));
    }
#ifdef NOS_ARM
    case 0xf0005: frame_set_tls(&current_task->frame, a[0]); return 0;
    case 403: case 406:
#endif
    case LNR(263, 113): case LNR(264, 114):
        if (a[0] > 7 || a[0] == 2 || a[0] == 3) return -NOS_EINVAL;
        if (!a[1]) return number == LNR(264, 114) ? 0 : -NOS_EFAULT;
        {
            uint64_t snapshot;
            int rc = clock_snapshot_ns(a[0], &snapshot); if (rc) return rc;
            if (number == LNR(264, 114) || number == 406) {
                snapshot = 10000000;
            }
            if (wide) { int64_t time[2] = { (int64_t)(snapshot / 1000000000), (int64_t)(snapshot % 1000000000) };
                return user_copy_out(a[1], time, sizeof(time)); }
            if (snapshot / 1000000000 > INT32_MAX) return -NOS_EOVERFLOW;
            int32_t time[2] = { (int32_t)(snapshot / 1000000000), (int32_t)(snapshot % 1000000000) };
            return user_copy_out(a[1], time, sizeof(time));
        }
#ifdef NOS_ARM
    case 407:
#endif
    case LNR(265, 115): return linux_sleep(a[0], a[1], a[2], a[3], wide);
    case LNR(162, 101): return linux_sleep(1, 0, a[0], a[1], sizeof(uintptr_t) == 8);
    case LNR(105, 102):
        if (a[0]) return -NOS_ENOTSUP;
        return timer_write(a[1], process_leader());
    case LNR(104, 103): {
        if (a[0]) return -NOS_ENOTSUP;
        uint64_t interval, value; int rc = timer_read(a[1], &interval, &value); if (rc) return rc;
        struct task *leader = process_leader(); if (a[2] && (rc = timer_write(a[2], leader))) return rc;
        leader->linux_alarm_interval = interval; leader->linux_alarm_deadline = value ? kernel_ticks + value : 0;
        return 0;
    }
#ifdef NOS_ARM
    case 27: {
        struct task *leader = process_leader();
        uint64_t remaining = leader->linux_alarm_deadline > kernel_ticks ? leader->linux_alarm_deadline - kernel_ticks : 0;
        if (a[0] > (UINT64_MAX - kernel_ticks) / 100) return -NOS_EINVAL;
        leader->linux_alarm_interval = 0; leader->linux_alarm_deadline = a[0] ? kernel_ticks + a[0] * 100 : 0;
        return (intptr_t)((remaining + 99) / 100);
    }
#endif
    case LNR(45, 214): return vm_linux_brk(current_task, a[0]);
    case LNR(192, 222): {
        if (!(a[3] & 0x20)) return gateway_enter(number, a);
        if (a[2] > 7 || a[3] > UINT32_MAX) return -NOS_EINVAL;
        return vm_map_linux(current_task, a[0], a[1], (unsigned)a[2], (unsigned)a[3]);
    }
    case LNR(91, 215): return vm_linux_unmap(current_task, a[0], a[1]);
    case LNR(125, 226): return a[2] > 7 ? -NOS_EINVAL : vm_linux_protect(current_task, a[0], (unsigned)a[2], a[1]);
    case LNR(144, 227): return a[2] > UINT32_MAX ? -NOS_EINVAL : vm_linux_sync(current_task, a[0], a[1], (unsigned)a[2]);
    case LNR(240, 98): return linux_futex(a, sizeof(uintptr_t) == 8);
#ifdef NOS_ARM
    case 422: return linux_futex(a, 1);
#endif
    case LNR(256, 96):
        if (a[0] && ((a[0] & 3) || !user_access(current_task, a[0], 4, 1))) return -NOS_EFAULT;
        current_task->clear_tid = a[0]; return current_task->id;
    case LNR(339, 100): {
        if (a[0] && a[0] != current_task->id) return -NOS_EPERM;
        uintptr_t size = 3 * sizeof(uintptr_t);
        if (user_copy_out(a[1], &current_task->linux_robust_list, sizeof(uintptr_t))) return -NOS_EFAULT;
        return user_copy_out(a[2], &size, sizeof(size));
    }
    case LNR(338, 99):
        if (a[1] != 3 * sizeof(uintptr_t) || !user_access(current_task, a[0], a[1], 0)) return -NOS_EINVAL;
        current_task->linux_robust_list = a[0]; return 0;
    case LNR(120, 220): return linux_clone(a);
    case LNR(369, 261): {
        int32_t pid = (int32_t)a[0];
        if (pid && (uint32_t)pid != current_task->space_owner) return -NOS_ESRCH;
        return process_limit_operation((unsigned)a[1], a[2], a[3], 1);
    }
#ifdef NOS_ARM
    case 191: return a[1] ? process_limit_operation((unsigned)a[0], 0, a[1], 0) : -NOS_EFAULT; /* ugetrlimit */
    case 75: case 76: return -NOS_ENOSYS; /* Legacy signed 32-bit rlimit ABI. */
#else
    case 163: case 164: return -NOS_ENOSYS; /* Use the implemented prlimit64 ABI. */
#endif
    case LNR(158, 124): reschedule = 1; return 0; /* sched_yield: no state change beyond rescheduling. */
    case LNR(398, 293): case 435: return -NOS_ENOSYS; /* rseq / clone3 have specified fallback paths. */
    default: return gateway_enter(number, a);
    }
}
#else
unsigned linux_process_group(const struct task *task) { (void)task; return 0; }
unsigned linux_process_session(const struct task *task) { (void)task; return 0; }
intptr_t linux_exec_current(uintptr_t request, uintptr_t entropy, uintptr_t flags)
{ (void)request; (void)entropy; (void)flags; return -NOS_ENOTSUP; }
void linux_exec_progress(void) { }
void linux_timer_tick(void) { }
int linux_wait_wake(struct task *waiter, struct task *child) { (void)waiter; (void)child; return 0; }
void linux_task_exit(struct task *task) { (void)task; }
int linux_task_prepare(struct task *task, uintptr_t entry) { (void)task; (void)entry; return -NOS_ENOTSUP; }
int linux_gateway_clone(struct task *child, const struct task *parent) { (void)child; (void)parent; return -NOS_ENOTSUP; }
intptr_t linux_gateway_return(uintptr_t result) { (void)result; return -NOS_ENOTSUP; }
intptr_t linux_dispatch(uintptr_t number, const uintptr_t arguments[6]) { (void)number; (void)arguments; return -NOS_ENOTSUP; }
#endif
