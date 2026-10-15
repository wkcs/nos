/* Copyright (C) 2026 NOS contributors */
#include "kernel.h"
struct task tasks[NOS_TASKS];
struct task *find_task(uintptr_t pid)
{
    if (pid > INT32_MAX) return NULL;
    struct task *t = &tasks[pid % NOS_TASKS];
    return t->id == pid && t->state != TASK_UNUSED && t->state != TASK_RETIRED ? t : NULL;
}
static void task_prepare(struct task *t, unsigned slot, unsigned priority, unsigned caps, unsigned parent)
{
    unsigned generation = t->generation;
    k_memset(t, 0, sizeof(*t)); t->generation = generation;
    t->id = generation * NOS_TASKS + slot;
    t->space_id = slot; t->space_owner = t->id;
    t->run_cpu = t->pin_cpu = UINT32_MAX;
    t->priority = t->base_priority = priority; t->caps = caps; t->parent = parent;
#ifndef NOS_MMU
    for (unsigned i = 0; i < NOS_TASKS; i++)
        if (tasks[i].state != TASK_UNUSED && tasks[i].state != TASK_DEAD) t->mutex_caps |= tasks[i].mutex_caps;
#endif
}
int create_task(unsigned slot, uintptr_t pc, uintptr_t arg, unsigned priority, unsigned caps, unsigned parent)
{
    struct task *t = &tasks[slot]; task_prepare(t, slot, priority, caps, parent);
    if (space_create(t)) { space_destroy(t); return -NOS_ENOMEM; }
#ifdef NOS_MMU
    uintptr_t stack = USER_DATA + USER_BYTES;
#else
    uintptr_t stack = t->memory + USER_BYTES;
#endif
    arch_frame_init(&t->frame, (uintptr_t)nos_process_start, stack, pc, arg);
    t->state = TASK_READY; return (int)t->id;
}
void task_reap(struct task *t)
{
    if (t->generation < ((unsigned)INT32_MAX - t->id % NOS_TASKS) / NOS_TASKS) {
        t->generation++; t->state = TASK_UNUSED;
    } else t->state = TASK_RETIRED;
}
intptr_t task_spawn(uintptr_t pc, uintptr_t arg, uintptr_t priority)
{
    if (!(current_task->caps & NOS_CAP_SPAWN)) return -NOS_EACCES;
    if (pc < (uintptr_t)__user_text_start || pc >= (uintptr_t)__user_text_end || priority > 30) return -NOS_EINVAL;
    for (unsigned i = 2; i < NOS_IDLE_FIRST; i++) {
        if (tasks[i].state != TASK_UNUSED) continue;
        int pid = create_task(i, pc, arg, (unsigned)priority, current_task->caps & ~NOS_CAP_SPAWN, current_task->id);
        if (pid >= 0) { tasks[i].mutex_caps |= current_task->mutex_caps; ipc_task_inherit(&tasks[i], current_task); }
        reschedule = 1; return pid;
    }
    return -NOS_EAGAIN;
}
intptr_t thread_create(uintptr_t pointer, uintptr_t priority)
{
    struct nos_thread_args args;
    if (user_copy_in(&args, pointer, sizeof(args))) return -NOS_EFAULT;
    if (priority == UINTPTR_MAX) priority = current_task->base_priority;
    if (priority > 30 || priority < current_task->base_priority || args.stack_size < 16 ||
        args.stack > UINTPTR_MAX - args.stack_size || (args.stack + args.stack_size) & 15u ||
        !user_exec_access(current_task, args.entry) ||
        !user_access(current_task, args.stack, args.stack_size, 1) ||
        (args.clear_tid && ((args.clear_tid & 3) || !user_access(current_task, args.clear_tid, sizeof(unsigned), 1))))
        return -NOS_EINVAL;
    for (unsigned i = 2; i < NOS_IDLE_FIRST; i++) {
        if (tasks[i].state != TASK_UNUSED) continue;
        struct task *t = &tasks[i];
        task_prepare(t, i, (unsigned)priority, current_task->caps & ~NOS_CAP_SPAWN, current_task->id);
        if (space_clone(t, current_task, args.stack + args.stack_size)) { space_destroy(t); return -NOS_ENOMEM; }
        arch_frame_init(&t->frame, args.entry, args.stack + args.stack_size, args.arg, 0);
#if defined(NOS_ARM64)
        t->frame.tls = args.tls;
#elif defined(NOS_RISCV)
        t->frame.x[4] = args.tls;
#endif
        if (args.parent_tid) {
            unsigned tid = t->id;
            if (task_copy_out(current_task, args.parent_tid, &tid, sizeof(tid))) {
                space_destroy(t); return -NOS_EFAULT;
            }
        }
        t->clear_tid = args.clear_tid; t->is_thread = 1;
        t->mutex_caps |= current_task->mutex_caps; ipc_task_inherit(t, current_task);
        t->state = TASK_READY; reschedule = 1; return t->id;
    }
    return -NOS_EAGAIN;
}
intptr_t task_wait(uintptr_t pid, uintptr_t status)
{
    struct task *child = find_task(pid);
    if (!child || child->parent != current_task->id) return -NOS_ESRCH;
    if (!user_access(current_task, status, sizeof(intptr_t), 1)) return -NOS_EFAULT;
    if (child->state == TASK_DEAD) {
        int rc = user_copy_out(status, &child->exit_status, sizeof(intptr_t)); task_reap(child); return rc;
    }
    current_task->pending = status; current_task->wait_for = (unsigned)pid;
    current_task->state = TASK_WAIT; reschedule = 1; return 0;
}
static int futex_load(struct task *task, uintptr_t address, unsigned *value)
{
    if ((address & 3) || !user_access(task, address, sizeof(unsigned), 0)) return -NOS_EFAULT;
    uintptr_t physical = task_user_phys_read(task, address);
    if (!physical) return -NOS_EFAULT;
    *value = __atomic_load_n((unsigned *)physical, __ATOMIC_ACQUIRE);
    return 0;
}
intptr_t futex_wait(uintptr_t address, uintptr_t expected, uintptr_t timeout)
{
    unsigned value;
    if (expected > UINT32_MAX || futex_load(current_task, address, &value)) return -NOS_EFAULT;
    if (value != (unsigned)expected) return -NOS_EAGAIN;
    if (timeout > 100000) return -NOS_EINVAL;
    current_task->futex_address = address; current_task->futex_value = (unsigned)expected;
    current_task->wake_tick = timeout ? kernel_ticks + timeout : 0;
    current_task->state = TASK_FUTEX; reschedule = 1; return 0;
}
static unsigned wake_for(struct task *caller, uintptr_t address, unsigned count)
{
    unsigned woken = 0;
    for (unsigned i = 0; i < NOS_TASKS && woken < count; i++) {
        struct task *t = &tasks[i];
        if (t->state == TASK_FUTEX && t->space_id == caller->space_id && t->futex_address == address) {
            t->state = TASK_READY; FRAME_RESULT(&t->frame) = 0; woken++;
        }
    }
    return woken;
}
intptr_t futex_wake(uintptr_t address, uintptr_t count)
{
    if ((address & 3) || !user_access(current_task, address, sizeof(unsigned), 0)) return -NOS_EFAULT;
    if (count > INT32_MAX) return -NOS_EINVAL;
    reschedule = 1; return wake_for(current_task, address, (unsigned)count);
}
intptr_t futex_requeue(uintptr_t request)
{
    struct nos_futex_requeue args;
    if (user_copy_in(&args, request, sizeof(args))) return -NOS_EFAULT;
    unsigned source, target;
    if (futex_load(current_task, args.source, &source) || futex_load(current_task, args.target, &target)) return -NOS_EFAULT;
    if (args.flags > 1 || args.source == args.target || args.wake_count > INT32_MAX || args.requeue_count > INT32_MAX)
        return -NOS_EINVAL;
    if (args.flags && source != args.compare) return -NOS_EAGAIN;
    unsigned affected = wake_for(current_task, args.source, args.wake_count), moved = 0;
    for (unsigned i = 0; i < NOS_TASKS && moved < args.requeue_count; i++) {
        struct task *waiter = &tasks[i];
        if (waiter->state == TASK_FUTEX && waiter->space_id == current_task->space_id && waiter->futex_address == args.source) {
            waiter->futex_address = args.target; moved++;
        }
    }
    reschedule = 1; return affected + moved;
}
void futex_task_exit(struct task *t)
{
    if (t->clear_tid) {
        if (!(t->clear_tid & 3) && user_access(t, t->clear_tid, sizeof(unsigned), 1)) {
            uintptr_t physical = task_user_phys(t, t->clear_tid);
            if (physical) {
                __atomic_store_n((unsigned *)physical, 0u, __ATOMIC_RELEASE);
                wake_for(t, t->clear_tid, UINT32_MAX);
            }
        }
        t->clear_tid = 0;
    }
}
void task_finish(struct task *t, intptr_t status)
{
    if (t->id % NOS_TASKS >= NOS_IDLE_FIRST) kernel_fail("idle task terminated");
    futex_task_exit(t); t->state = TASK_DEAD; mutex_task_exit(t); ipc_task_exit(t);
    if (t->id % NOS_TASKS == 0) {
        for (unsigned i = 2; i < NOS_IDLE_FIRST; i++) {
            struct task *peer = &tasks[i];
            if (peer->space_owner != t->space_owner || peer->state == TASK_UNUSED ||
                peer->state == TASK_DEAD || peer->state == TASK_RETIRED) continue;
            if (peer->run_cpu != UINT32_MAX && peer->run_cpu != arch_cpu_id()) peer->cancel_pending = 1;
            else task_finish(peer, status);
        }
    }
    mutex_recompute();
    irq_task_exit(t);
    unsigned group_alive = 0;
    for (unsigned i = 0; i < NOS_TASKS; i++) {
        struct task *peer = &tasks[i];
        if (peer != t && peer->space_owner == t->space_owner && peer->state != TASK_UNUSED &&
            peer->state != TASK_DEAD && peer->state != TASK_RETIRED) group_alive = 1;
    }
    if ((t->caps & NOS_CAP_DEVICE) && !group_alive) arch_device_revoke(t);
    space_destroy(t); t->exit_status = status; t->state = TASK_DEAD;
    unsigned reaped = 0;
    for (unsigned i = 0; i < NOS_TASKS; i++) {
        struct task *w = &tasks[i];
        if (w->state == TASK_WAIT && w->wait_for == t->id) {
            int rc = task_copy_out(w, w->pending, &status, sizeof(status));
            FRAME_RESULT(&w->frame) = (uintptr_t)rc; w->state = TASK_READY; reaped = 1;
        }
        if (w != t && w->parent == t->id) {
            /* Orphans are detached, never consume unreapable zombie slots. */
            w->parent = UINT32_MAX;
            if (w->state == TASK_DEAD) task_reap(w);
        }
    }
    struct task *parent = find_task(t->parent);
    if (reaped || t->is_thread || !parent || parent->state == TASK_DEAD) task_reap(t);
    reschedule = 1;
}
