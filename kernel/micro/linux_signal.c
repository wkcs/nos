/* Copyright (C) 2026 NOS contributors */
#include "kernel.h"
#if defined(NOS_MMU) && defined(NOS_LINUX_COMPAT)
#ifdef NOS_ARM
#define NR(arm, generic) (arm)
#define UC_OFFSET 128u
#define UC_MASK 104u
#define UC_CONTEXT 20u
#define SIGNAL_FRAME_BYTES 872u
#define INFO_PAYLOAD 12u
#else
#define NR(arm, generic) (generic)
#define UC_OFFSET 128u
#define UC_MASK 40u
#define UC_CONTEXT 176u
#define INFO_PAYLOAD 16u
#ifdef NOS_ARM64
#define SIGNAL_FRAME_BYTES 4688u
#else
#define SIGNAL_FRAME_BYTES 1088u
#endif
#endif
#define QUEUED_SIGNALS 32u
#define SIGNAL_DEPTH 8u
#define SIGRETURN_PAGE 0x018ff000u
#define SA_SIGINFO 4u
#define SA_RESTORER 0x04000000u
#define SA_ONSTACK 0x08000000u
#define SA_RESTART 0x10000000u
#define SA_NODEFER 0x40000000u
#define SA_RESETHAND 0x80000000u
struct signal_action {
    uintptr_t handler, flags;
#ifndef NOS_RISCV
    uintptr_t restorer;
#endif
#ifdef NOS_ARM
    uint32_t mask[2];
#else
    uint64_t mask;
#endif
};
struct signal_stack { uintptr_t base; int flags;
#if UINTPTR_MAX > UINT32_MAX
    unsigned padding;
#endif
    size_t size;
};
struct signal_queue { unsigned signal; unsigned char info[128]; };
struct signal_frame { struct arch_frame original; uint64_t mask; uintptr_t address; unsigned long cookie; };
static struct signal_action actions[NOS_SPACES][64];
static struct signal_queue queues[NOS_TASKS][QUEUED_SIGNALS];
static struct signal_frame frames[NOS_TASKS][SIGNAL_DEPTH];
static unsigned action_owners[NOS_SPACES], action_valid[NOS_SPACES];
static unsigned signal_space(const struct task *task)
{
    if (task->space_id >= NOS_SPACES) kernel_fail("invalid signal address space");
    return task->space_id;
}
static struct signal_action *task_actions(const struct task *task)
{ return actions[signal_space(task)]; }
static void store32(void *buffer, unsigned offset, uint32_t value) { k_memcpy((char *)buffer + offset, &value, 4); }
static void store64(void *buffer, unsigned offset, uint64_t value) { k_memcpy((char *)buffer + offset, &value, 8); }
static uint32_t load32(const void *buffer, unsigned offset) { uint32_t value; k_memcpy(&value, (const char *)buffer + offset, 4); return value; }
static uint64_t load64(const void *buffer, unsigned offset) { uint64_t value; k_memcpy(&value, (const char *)buffer + offset, 8); return value; }
static uintptr_t get_sp(const struct arch_frame *frame)
{
#ifdef NOS_RISCV
    return frame->x[2];
#else
    return frame->sp;
#endif
}
static void put_sp(struct arch_frame *frame, uintptr_t value)
{
#ifdef NOS_RISCV
    frame->x[2] = value;
#else
    frame->sp = value;
#endif
}
static void put_args(struct arch_frame *frame, uintptr_t a, uintptr_t b, uintptr_t c)
{
#ifdef NOS_ARM64
    frame->x[0] = a; frame->x[1] = b; frame->x[2] = c;
#elif defined(NOS_RISCV)
    frame->x[10] = a; frame->x[11] = b; frame->x[12] = c;
#else
    frame->r[0] = a; frame->r[1] = b; frame->r[2] = c;
#endif
}
static void put_link(struct arch_frame *frame, uintptr_t link)
{
#ifdef NOS_ARM64
    frame->x[30] = link;
#elif defined(NOS_RISCV)
    frame->x[1] = link;
#else
    frame->lr = link;
#endif
}
static uint64_t action_mask(const struct signal_action *action)
{
    uint64_t mask; k_memcpy(&mask, &action->mask, 8); return mask;
}
static int on_altstack(const struct task *task, uintptr_t sp)
{ return task->signal_stack_size && sp >= task->signal_stack_base && sp - task->signal_stack_base < task->signal_stack_size; }
static int ignored_default(unsigned signal)
{ return signal == 17 || signal == 23 || signal == 28 || signal == 18; }
static void refresh_pending(struct task *task)
{
    task->signal_pending = 0;
    for (unsigned i = 0; i < QUEUED_SIGNALS; i++) {
        unsigned signal = queues[task->id % NOS_TASKS][i].signal;
        if (signal) task->signal_pending |= 1ull << (signal - 1);
    }
}
static int dequeue(struct task *task, uint64_t allowed, unsigned char info[128])
{
    uint64_t pending = task->signal_pending & allowed;
    if (!pending) return 0;
    unsigned signal = 1; while (!(pending & (1ull << (signal - 1)))) signal++;
    struct signal_queue *queue = queues[task->id % NOS_TASKS];
    for (unsigned i = 0; i < QUEUED_SIGNALS; i++) if (queue[i].signal == signal) {
        k_memcpy(info, queue[i].info, 128);
        for (unsigned n = i + 1; n < QUEUED_SIGNALS; n++) queue[n - 1] = queue[n];
        k_memset(&queue[QUEUED_SIGNALS - 1], 0, sizeof(queue[0])); refresh_pending(task); return (int)signal;
    }
    return 0;
}
static void group_stop(struct task *target, int stopped)
{
    for (unsigned i = 0; i < NOS_TASKS; i++) if (tasks[i].space_owner == target->space_owner &&
        tasks[i].state != TASK_UNUSED && tasks[i].state != TASK_DEAD) tasks[i].linux_stopped = (unsigned)stopped;
    reschedule = 1;
}
static void terminate_group(struct task *target, unsigned signal)
{
    unsigned group = target->space_owner;
    for (unsigned i = 2; i < NOS_IDLE_FIRST; i++) {
        struct task *peer = &tasks[i];
        if (peer == target || peer->space_owner != group || peer->state == TASK_UNUSED || peer->state == TASK_DEAD) continue;
        peer->linux_exit_signal = signal;
        if (peer->run_cpu != UINT32_MAX && peer->run_cpu != arch_cpu_id()) peer->cancel_pending = 1;
        else task_finish(peer, -(intptr_t)signal);
    }
    target->linux_exit_signal = signal; task_finish(target, -(intptr_t)signal);
}
static void discard_group_signals(struct task *target, uint64_t mask)
{
    for (unsigned n = 0; n < NOS_TASKS; n++) {
        struct task *peer = &tasks[n]; if (peer->space_owner != target->space_owner) continue;
        struct signal_queue *queue = queues[peer->id % NOS_TASKS];
        unsigned used = 0;
        for (unsigned i = 0; i < QUEUED_SIGNALS; i++) if (queue[i].signal && !(mask & (1ull << (queue[i].signal - 1))))
            queue[used++] = queue[i];
        k_memset(&queue[used], 0, (QUEUED_SIGNALS - used) * sizeof(queue[0])); refresh_pending(peer);
    }
}
void linux_signal_reset(struct task *task)
{
    unsigned space = signal_space(task);
    if (!action_valid[space] || action_owners[space] != task->space_owner) {
        k_memset(actions[space], 0, sizeof(actions[0]));
        action_valid[space] = 1; action_owners[space] = task->space_owner;
    }
    k_memset(queues[task->id % NOS_TASKS], 0, sizeof(queues[0]));
    task->signal_pending = 0; task->signal_depth = 0;
}
void linux_signal_fork(struct task *child, const struct task *parent)
{
    unsigned space = signal_space(child);
    k_memcpy(actions[space], task_actions(parent), sizeof(actions[0]));
    action_valid[space] = 1; action_owners[space] = child->space_owner;
    child->signal_mask = parent->signal_mask;
    child->signal_stack_base = parent->signal_stack_base; child->signal_stack_size = parent->signal_stack_size;
    child->signal_depth = parent->signal_depth;
    for (unsigned i = 0; i < child->signal_depth; i++) {
        frames[child->id % NOS_TASKS][i] = frames[parent->id % NOS_TASKS][i];
        frames[child->id % NOS_TASKS][i].cookie = child->id ^ i ^ 0x53494746u;
    }
}
void linux_signal_discard(struct task *task)
{ k_memset(queues[task->id % NOS_TASKS], 0, sizeof(queues[0])); task->signal_pending = 0; task->signal_depth = 0; }
/* fcntl is multiplexed. Only blocking POSIX record-lock requests restart;
 * the saved frame describes the Linux caller rather than its native gateway. */
static int interrupted_record_lock_wait(const struct task *task)
{
#ifdef NOS_ARM
    uintptr_t command = task->linux_syscall_frame.r[1];
    return (task->linux_syscall_number == 55 || task->linux_syscall_number == 221) &&
        (command == 7 || command == 14);
#else
#ifdef NOS_ARM64
    uintptr_t command = task->linux_syscall_frame.x[1];
#else
    uintptr_t command = task->linux_syscall_frame.x[11];
#endif
    return task->linux_syscall_number == 25 && command == 7;
#endif
}
void linux_signal_interrupted(struct task *task)
{
    uintptr_t number = task->linux_syscall_number;
    if (number == NR(3, 63) || number == NR(4, 64) || number == NR(145, 65) || number == NR(146, 66) ||
        number == NR(322, 56) || number == NR(114, 260) || number == NR(240, 98) ||
        interrupted_record_lock_wait(task))
        task->linux_restart_pending = 1;
}
void linux_signal_exec(struct task *task, unsigned old_space)
{
    unsigned space = signal_space(task);
    if (old_space >= NOS_SPACES) kernel_fail("invalid previous signal address space");
    if (space != old_space) k_memcpy(actions[space], actions[old_space], sizeof(actions[0]));
    for (unsigned i = 0; i < 64; i++) if (actions[space][i].handler != 1)
        k_memset(&actions[space][i], 0, sizeof(actions[0][i]));
    action_valid[space] = 1; action_owners[space] = task->space_owner;
    task->signal_depth = task->signal_suspend = 0; task->signal_stack_base = task->signal_stack_size = 0;
}
void linux_signal_retid(struct task *source, struct task *destination)
{
    if (source == destination) return;
    k_memcpy(queues[destination->id % NOS_TASKS], queues[source->id % NOS_TASKS], sizeof(queues[0]));
    k_memset(queues[source->id % NOS_TASKS], 0, sizeof(queues[0]));
}
/* Both enqueue and the blocking admission check hold kernel_lock. Keep the
 * interruption result, restart decision and wait cleanup identical whether a
 * signal arrives before or after the task publishes its blocked state. */
static void interrupt_wait(struct task *task)
{
    enum task_state previous_state = task->state;
    if (task->state == TASK_REPLY || task->state == TASK_RECV) ipc_interrupt_task(task, -NOS_EINTR);
    else if (task->state == TASK_SLEEP || task->state == TASK_FUTEX || task->state == TASK_SIGNAL_WAIT || task->state == TASK_WAIT) {
        if (task->abi == NOS_ABI_LINUX) linux_signal_interrupted(task);
        task->linux_wait = 0;
        if (task->state == TASK_SLEEP && task->linux_sleep_remain) {
            uint64_t remain = task->wake_tick > kernel_ticks ? task->wake_tick - kernel_ticks : 0;
            if (task->linux_sleep_wide) {
                int64_t time[2] = { (int64_t)(remain / 100), (int64_t)(remain % 100) * 10000000 };
                (void)task_copy_out(task, task->linux_sleep_remain, time, sizeof(time));
            } else {
                int32_t time[2] = { (int32_t)(remain / 100), (int32_t)(remain % 100) * 10000000 };
                (void)task_copy_out(task, task->linux_sleep_remain, time, sizeof(time));
            }
            task->linux_sleep_remain = 0;
        }
        FRAME_RESULT(&task->frame) = (uintptr_t)-NOS_EINTR; task->state = TASK_READY;
    }
    if (previous_state == TASK_FUTEX && task->state != TASK_FUTEX) vm_shared_collect();
    if (previous_state != task->state) reschedule = 1;
}
void linux_signal_interrupt_pending(struct task *task)
{
    if ((task->abi == NOS_ABI_LINUX || task->abi == NOS_ABI_GATEWAY) &&
        task->signal_pending & (~task->signal_mask | (1ull << 8) | (1ull << 18)))
        interrupt_wait(task);
}
static int signal_send_uid(struct task *task, unsigned signal, int code, unsigned sender, uint32_t uid, uintptr_t value)
{
    if (!task || task->state == TASK_UNUSED || task->state == TASK_DEAD || task->state == TASK_RETIRED || task->abi == NOS_ABI_NATIVE) return -NOS_ESRCH;
    if (signal > 64) return -NOS_EINVAL;
    if (!signal) return 0;
    if (signal == 18) {
        discard_group_signals(task, (1ull << 18) | (1ull << 19) | (1ull << 20) | (1ull << 21));
        group_stop(task, 0);
    } else if (signal == 19 || signal == 20 || signal == 21 || signal == 22) discard_group_signals(task, 1ull << 17);
    if (signal == 9) group_stop(task, 0);
    struct signal_action *action = &task_actions(task)[signal - 1];
    if (signal != 9 && signal != 19 && (action->handler == 1 || (!action->handler && ignored_default(signal)))) return 0;
    struct signal_queue *queue = queues[task->id % NOS_TASKS];
    unsigned index = QUEUED_SIGNALS;
    for (unsigned i = 0; i < QUEUED_SIGNALS; i++) {
        if (signal < 32 && queue[i].signal == signal) return 0;
        if (!queue[i].signal && index == QUEUED_SIGNALS) index = i;
    }
    if (index == QUEUED_SIGNALS) return -NOS_EAGAIN;
    k_memset(&queue[index], 0, sizeof(queue[index])); queue[index].signal = signal;
    store32(queue[index].info, 0, signal); store32(queue[index].info, 8, (uint32_t)code);
    if (code > 0 && (signal == 4 || signal == 7 || signal == 8 || signal == 11)) {
        if (sizeof(uintptr_t) == 8) store64(queue[index].info, INFO_PAYLOAD, value);
        else store32(queue[index].info, INFO_PAYLOAD, (uint32_t)value);
    } else {
        store32(queue[index].info, INFO_PAYLOAD, sender); store32(queue[index].info, INFO_PAYLOAD + 4, uid);
        if (sizeof(uintptr_t) == 8) store64(queue[index].info, INFO_PAYLOAD + 8, value);
        else store32(queue[index].info, INFO_PAYLOAD + 8, (uint32_t)value);
    }
    task->signal_pending |= 1ull << (signal - 1);
    if (task->state == TASK_SIGNAL_WAIT && task->signal_wait_set & (1ull << (signal - 1))) {
        unsigned char info[128]; int received = dequeue(task, task->signal_wait_set, info);
        int rc = task->signal_wait_info ? task_copy_out(task, task->signal_wait_info, info, 128) : 0;
        FRAME_RESULT(&task->frame) = (uintptr_t)(rc ? rc : received); task->state = TASK_READY;
    } else if (!(task->signal_mask & (1ull << (signal - 1))) || signal == 9 || signal == 19) {
        interrupt_wait(task);
    }
    reschedule = 1; return 0;
}
/* Trusted kernel events (faults, timers and SIGCHLD) do not require a user
 * permission check. Capture the sender UID now, before task_reap can free it. */
int linux_signal_send(struct task *task, unsigned signal, int code, unsigned sender, uintptr_t value)
{
    struct task *source = sender ? find_task(sender) : NULL;
    uint32_t uid = source ? credentials_get(source)->ruid : 0;
    return signal_send_uid(task, signal, code, sender, uid, value);
}
static int user_signal_send(struct task *target, unsigned signal, int code, uintptr_t value)
{
    if (signal > 64) return -NOS_EINVAL;
    if (!target || target->state == TASK_UNUSED || target->state == TASK_DEAD ||
        target->state == TASK_RETIRED || target->abi == NOS_ABI_NATIVE) return -NOS_ESRCH;
    if (!credentials_signal_allowed(current_task, target, signal)) return -NOS_EPERM;
    return signal_send_uid(target, signal, code, current_task->space_owner, credentials_get(current_task)->ruid, value);
}
static void fill_context(unsigned char *frame, struct task *task, const struct arch_frame *saved, uint64_t mask)
{
    unsigned char *uc = frame + UC_OFFSET;
#ifdef NOS_ARM
    store32(uc, 8, (uint32_t)task->signal_stack_base);
    store32(uc, 12, !task->signal_stack_size ? 2 : on_altstack(task, get_sp(saved)) ? 1 : 0);
    store32(uc, 16, (uint32_t)task->signal_stack_size);
    unsigned char *sc = uc + UC_CONTEXT;
    store32(sc, 8, (uint32_t)mask);
    for (unsigned i = 0; i < 13; i++) store32(sc, 12 + i * 4, (uint32_t)saved->r[i]);
    store32(sc, 64, (uint32_t)saved->sp); store32(sc, 68, (uint32_t)saved->lr);
    store32(sc, 72, (uint32_t)saved->pc); store32(sc, 76, (uint32_t)saved->status);
    store32(uc, 232, 0x56465001u); store32(uc, 236, 288);
    k_memcpy(uc + 240, saved->d, sizeof(saved->d)); store32(uc, 496, (uint32_t)saved->fpscr);
    store32(uc, 504, 1u << 30);
#else
    store64(uc, 16, task->signal_stack_base);
    store32(uc, 24, !task->signal_stack_size ? 2 : on_altstack(task, get_sp(saved)) ? 1 : 0);
    store64(uc, 32, task->signal_stack_size);
    unsigned char *sc = uc + UC_CONTEXT;
#ifdef NOS_ARM64
    for (unsigned i = 0; i < 31; i++) store64(sc, 8 + i * 8, saved->x[i]);
    store64(sc, 256, saved->sp); store64(sc, 264, saved->pc); store64(sc, 272, saved->status);
    store32(sc, 288, 0x46508001u); store32(sc, 292, 528);
    store32(sc, 296, (uint32_t)saved->fpsr); store32(sc, 300, (uint32_t)saved->fpcr);
    k_memcpy(sc + 304, saved->q, sizeof(saved->q));
#else
    store64(sc, 0, saved->pc);
    for (unsigned i = 1; i < 32; i++) store64(sc, i * 8, saved->x[i]);
    k_memcpy(sc + 256, saved->f, sizeof(saved->f)); store32(sc, 512, (uint32_t)saved->fcsr);
#endif
#endif
    store64(uc, UC_MASK, mask);
}
static int restore_context(struct task *task, struct arch_frame *restored, const unsigned char *frame)
{
    const unsigned char *uc = frame + UC_OFFSET, *sc = uc + UC_CONTEXT;
#ifdef NOS_ARM
    for (unsigned i = 0; i < 13; i++) restored->r[i] = load32(sc, 12 + i * 4);
    restored->sp = load32(sc, 64); restored->lr = load32(sc, 68); restored->pc = load32(sc, 72);
    uintptr_t status = load32(sc, 76);
    if ((status & 31) != 0x10) return -NOS_EFAULT;
    restored->status = (restored->status & ~(uintptr_t)0xf8000020u) | (status & 0xf8000020u);
    if (load32(uc, 232) != 0x56465001u || load32(uc, 236) != 288) return -NOS_EFAULT;
    k_memcpy(restored->d, uc + 240, sizeof(restored->d)); restored->fpscr = load32(uc, 496);
#elif defined(NOS_ARM64)
    for (unsigned i = 0; i < 31; i++) restored->x[i] = load64(sc, 8 + i * 8);
    restored->sp = load64(sc, 256); restored->pc = load64(sc, 264);
    if (load64(sc, 272) & 15) return -NOS_EFAULT;
    restored->status = (restored->status & ~(uintptr_t)0xf0000000u) | (load64(sc, 272) & 0xf0000000u);
    if (load32(sc, 288) != 0x46508001u || load32(sc, 292) != 528) return -NOS_EFAULT;
    restored->fpsr = load32(sc, 296); restored->fpcr = load32(sc, 300); k_memcpy(restored->q, sc + 304, sizeof(restored->q));
#else
    restored->pc = load64(sc, 0);
    for (unsigned i = 1; i < 32; i++) restored->x[i] = load64(sc, i * 8);
    k_memcpy(restored->f, sc + 256, sizeof(restored->f)); restored->fcsr = load32(sc, 512);
#endif
    uintptr_t pc = restored->pc;
#ifdef NOS_ARM
    if (restored->status & (1u << 5)) pc |= 1;
#endif
    uintptr_t sp = get_sp(restored);
    /* ARM EABI wrappers can push just r7 before SVC. A signal may therefore
     * interrupt a valid instruction with SP 4 mod 8; handler entry itself
     * remains aligned, but sigreturn must restore that interrupted context. */
#ifdef NOS_ARM
    const uintptr_t stack_mask = 3;
#else
    const uintptr_t stack_mask = 15;
#endif
    if (!sp || (sp & stack_mask) || !user_exec_access(task, pc) || !user_access(task, sp - 1, 1, 1)) return -NOS_EFAULT;
    task->signal_mask = load64(uc, UC_MASK) & ~((1ull << 8) | (1ull << 18));
    return 0;
}
static int install_restorer(struct task *task)
{
    if (user_exec_access(task, SIGRETURN_PAGE)) return 0;
#ifdef NOS_ARM64
    const uint32_t instructions[] = { 0xd2801168u, 0xd4000001u, 0xd4200000u };
#elif defined(NOS_ARM)
    const uint32_t instructions[] = { 0xe3a070adu, 0xef000000u, 0xe7f000f0u };
#else
    const uint32_t instructions[] = { 0x08b00893u, 0x00000073u, 0x00100073u };
#endif
    int rc = vm_gateway_map(task, SIGRETURN_PAGE, NOS_PAGE_SIZE, NOS_PROT_READ | NOS_PROT_EXEC);
    if (rc) return rc;
    rc = task_copy_load(task, SIGRETURN_PAGE, instructions, sizeof(instructions)); arch_cache_sync(); return rc;
}
void linux_signal_deliver(struct task *task)
{
    if (task->abi != NOS_ABI_LINUX || task->state != TASK_READY || !task->signal_pending) return;
    uint64_t allowed = ~task->signal_mask | (1ull << 8) | (1ull << 18);
    unsigned char info[128]; int signal = dequeue(task, allowed, info); if (!signal) return;
    struct signal_action action = task_actions(task)[signal - 1];
    if (signal == 9 || (!action.handler && !ignored_default((unsigned)signal) && signal != 19 && signal != 20 && signal != 21 && signal != 22)) {
        terminate_group(task, (unsigned)signal); return;
    }
    if (signal == 19 || (!action.handler && (signal == 20 || signal == 21 || signal == 22))) { group_stop(task, 1); return; }
    if (action.handler <= 1) return;
    if (task->signal_depth == SIGNAL_DEPTH || !user_exec_access(task, action.handler) || install_restorer(task)) { terminate_group(task, 11); return; }
    struct signal_frame *record = &frames[task->id % NOS_TASKS][task->signal_depth];
    record->original = task->frame;
    if ((action.flags & SA_RESTART) && task->linux_restart_pending) {
        record->original = task->linux_syscall_frame;
#ifdef NOS_ARM
        record->original.pc -= record->original.status & (1u << 5) ? 2 : 4;
#else
        record->original.pc -= 4;
#endif
    }
    task->linux_restart_pending = 0;
    record->mask = task->signal_suspend ? task->signal_restore_mask : task->signal_mask;
    uintptr_t sp = get_sp(&task->frame);
    if ((action.flags & SA_ONSTACK) && task->signal_stack_size && !on_altstack(task, sp)) sp = task->signal_stack_base + task->signal_stack_size;
    if (sp < SIGNAL_FRAME_BYTES + 16) { terminate_group(task, 11); return; }
    record->address = (sp - SIGNAL_FRAME_BYTES) & ~(uintptr_t)15;
    record->cookie = task->id ^ task->signal_depth ^ 0x53494746u;
    unsigned char frame[SIGNAL_FRAME_BYTES]; k_memset(frame, 0, sizeof(frame)); k_memcpy(frame, info, 128);
    fill_context(frame, task, &record->original, record->mask);
    if (task_copy_out(task, record->address, frame, sizeof(frame))) { terminate_group(task, 11); return; }
    task->signal_depth++; task->signal_suspend = 0;
    task->signal_mask |= action_mask(&action);
    if (!(action.flags & SA_NODEFER)) task->signal_mask |= 1ull << (signal - 1);
    task->signal_mask &= ~((1ull << 8) | (1ull << 18));
    if (action.flags & SA_RESETHAND) k_memset(&task_actions(task)[signal - 1], 0, sizeof(action));
    task->frame.pc = action.handler;
#ifdef NOS_ARM
    if (task->frame.pc & 1) task->frame.status |= 1u << 5; else task->frame.status &= ~(1u << 5);
    task->frame.pc &= ~(uintptr_t)1;
#endif
    put_sp(&task->frame, record->address);
    put_args(&task->frame, (unsigned)signal, record->address, record->address + UC_OFFSET);
    uintptr_t restorer = SIGRETURN_PAGE;
#ifndef NOS_RISCV
    if ((action.flags & SA_RESTORER) && action.restorer) restorer = action.restorer;
#endif
    put_link(&task->frame, restorer);
}
static intptr_t signal_return(void)
{
    struct task *task = current_task;
    if (!task->signal_depth) { terminate_group(task, 11); return 0; }
    struct signal_frame *record = &frames[task->id % NOS_TASKS][task->signal_depth - 1];
    if (get_sp(&task->frame) != record->address || record->cookie != (task->id ^ (task->signal_depth - 1) ^ 0x53494746u)) {
        terminate_group(task, 11); return 0;
    }
    unsigned char frame[SIGNAL_FRAME_BYTES]; struct arch_frame restored = record->original;
    if (user_copy_in(frame, record->address, sizeof(frame)) || restore_context(task, &restored, frame)) { terminate_group(task, 11); return 0; }
    task->signal_depth--; task->frame = restored;
    return (intptr_t)FRAME_RESULT(&task->frame);
}
static int signal_target(uintptr_t pid, unsigned signal, int code, uintptr_t value)
{
    struct task *selected = NULL;
    for (unsigned i = 2; i < NOS_IDLE_FIRST; i++) {
        struct task *task = &tasks[i];
        if (task->space_owner != pid || task->abi == NOS_ABI_NATIVE || task->state == TASK_UNUSED || task->state == TASK_DEAD || task->state == TASK_RETIRED) continue;
        if (!selected || !(task->signal_mask & (1ull << (signal ? signal - 1 : 0)))) selected = task;
        if (signal && !(task->signal_mask & (1ull << (signal - 1)))) break;
    }
    return user_signal_send(selected, signal, code, value);
}
void linux_signal_tick(void)
{
    for (unsigned i = 2; i < NOS_IDLE_FIRST; i++) if (tasks[i].state == TASK_SIGNAL_WAIT && tasks[i].wake_tick && tasks[i].wake_tick <= kernel_ticks) {
        FRAME_RESULT(&tasks[i].frame) = (uintptr_t)-NOS_EAGAIN; tasks[i].state = TASK_READY;
    }
}
void linux_wait_control_end(struct task *task)
{
    if (!task->linux_waitcontrol) return;
    task->linux_waitcontrol = 0;
    if (task->signal_pending & ~task->signal_mask) {
        task->signal_suspend = 1; task->signal_restore_mask = task->linux_waitcontrol_mask;
    } else task->signal_mask = task->linux_waitcontrol_mask;
}
intptr_t linux_wait_control(uintptr_t operation, uintptr_t argument, uintptr_t size)
{
    struct task *task = current_task;
    if (task->abi != NOS_ABI_GATEWAY || !task->linux_gateway_active) return -NOS_EPERM;
    if (operation == NOS_LINUX_WAIT_BEGIN) {
        if (task->linux_waitcontrol || size != 8) return -NOS_EINVAL;
        uint64_t mask = task->signal_mask;
        if (argument && user_copy_in(&mask, argument, sizeof(mask))) return -NOS_EFAULT;
        task->linux_waitcontrol_mask = task->signal_mask; task->linux_waitcontrol = 1;
        task->signal_mask = mask & ~((1ull << 8) | (1ull << 18));
        return task->signal_pending & ~task->signal_mask ? -NOS_EINTR : 0;
    }
    if (!task->linux_waitcontrol) return -NOS_EINVAL;
    if (operation == NOS_LINUX_WAIT_END) { linux_wait_control_end(task); return 0; }
    if (operation != NOS_LINUX_WAIT_SLEEP || size || argument > UINT64_MAX - kernel_ticks) return -NOS_EINVAL;
    if (task->signal_pending & ~task->signal_mask) return -NOS_EINTR;
    if (argument) { task->wake_tick = kernel_ticks + argument; task->state = TASK_SLEEP; reschedule = 1; }
    return 0;
}
intptr_t linux_signal_syscall(uintptr_t number, const uintptr_t a[6])
{
    switch (number) {
    case NR(173, 139): return signal_return();
    case NR(174, 134): {
        if (!a[0] || a[0] > 64 || a[0] == 9 || a[0] == 19 || a[3] != 8) return -NOS_EINVAL;
        struct signal_action *action = &task_actions(current_task)[a[0] - 1], next;
        if (a[1] && user_copy_in(&next, a[1], sizeof(next))) return -NOS_EFAULT;
        if (a[2] && user_copy_out(a[2], action, sizeof(*action))) return -NOS_EFAULT;
        if (a[1]) *action = next;
        return 0;
    }
    case NR(175, 135): {
        if (a[3] != 8) return -NOS_EINVAL;
        uint64_t old = current_task->signal_mask, next;
        if (a[1] && user_copy_in(&next, a[1], 8)) return -NOS_EFAULT;
        if (a[2] && user_copy_out(a[2], &old, 8)) return -NOS_EFAULT;
        if (a[1]) {
            next &= ~((1ull << 8) | (1ull << 18));
            if (a[0] == 0) current_task->signal_mask |= next;
            else if (a[0] == 1) current_task->signal_mask &= ~next;
            else if (a[0] == 2) current_task->signal_mask = next;
            else return -NOS_EINVAL;
        }
        return 0;
    }
    case NR(176, 136):
        if (a[1] != 8) return -NOS_EINVAL;
        return user_copy_out(a[0], &current_task->signal_pending, 8);
    case NR(186, 132): {
        struct signal_stack old; k_memset(&old, 0, sizeof(old));
        old.base = current_task->signal_stack_base; old.size = current_task->signal_stack_size;
        old.flags = !old.size ? 2 : on_altstack(current_task, get_sp(&current_task->frame)) ? 1 : 0;
        struct signal_stack next;
        if (a[0] && user_copy_in(&next, a[0], sizeof(next))) return -NOS_EFAULT;
        if (a[1] && user_copy_out(a[1], &old, sizeof(old))) return -NOS_EFAULT;
        if (a[0]) {
            if (old.flags == 1) return -NOS_EPERM;
            if (next.flags & ~2) return -NOS_EINVAL;
            if (!(next.flags & 2) && (next.size < SIGNAL_FRAME_BYTES + 256 || !user_access(current_task, next.base, next.size, 1))) return -NOS_ENOMEM;
            current_task->signal_stack_base = next.flags & 2 ? 0 : next.base;
            current_task->signal_stack_size = next.flags & 2 ? 0 : next.size;
        }
        return 0;
    }
    case NR(37, 129): {
        if (a[1] > 64) return -NOS_EINVAL;
        intptr_t pid = (intptr_t)a[0];
        if (pid > 0) return signal_target((uintptr_t)pid, (unsigned)a[1], 0, 0);
        if (pid < -INT32_MAX) return -NOS_ESRCH;
        unsigned group = pid == 0 ? linux_process_group(current_task) : pid == -1 ? 0 : (unsigned)(-(pid + 1)) + 1;
        int found = 0, delivered = 0, error = -NOS_ESRCH;
        for (unsigned i = 2; i < NOS_IDLE_FIRST; i++) {
            struct task *leader = &tasks[i];
            if (leader->state == TASK_UNUSED || leader->state == TASK_RETIRED || leader->abi == NOS_ABI_NATIVE ||
                leader->id != leader->space_owner || (group && linux_process_group(leader) != group) ||
                (!group && leader->id == current_task->space_owner)) continue;
            int rc = signal_target(leader->id, (unsigned)a[1], 0, 0);
            if (rc != -NOS_ESRCH) { found = 1; error = rc; }
            if (!rc) delivered = 1;
        }
        return delivered ? 0 : found ? error : -NOS_ESRCH;
    }
    case NR(238, 130): {
        struct task *target = find_task(a[0]);
        return user_signal_send(target, (unsigned)a[1], -6, 0);
    }
    case NR(268, 131): {
        struct task *target = find_task(a[1]);
        if (!target || target->space_owner != a[0]) return -NOS_ESRCH;
        return user_signal_send(target, (unsigned)a[2], -6, 0);
    }
    case NR(178, 138): {
        if (a[1] > 64) return -NOS_EINVAL;
        unsigned char info[128]; if (user_copy_in(info, a[2], sizeof(info))) return -NOS_EFAULT;
        uintptr_t value = sizeof(uintptr_t) == 8 ? (uintptr_t)load64(info, INFO_PAYLOAD + 8) : load32(info, INFO_PAYLOAD + 8);
        return signal_target(a[0], (unsigned)a[1], -1, value);
    }
    case NR(363, 240): {
        if (a[2] > 64) return -NOS_EINVAL;
        struct task *target = find_task(a[1]); if (!target || target->space_owner != a[0]) return -NOS_ESRCH;
        unsigned char info[128]; if (user_copy_in(info, a[3], sizeof(info))) return -NOS_EFAULT;
        uintptr_t value = sizeof(uintptr_t) == 8 ? (uintptr_t)load64(info, INFO_PAYLOAD + 8) : load32(info, INFO_PAYLOAD + 8);
        return user_signal_send(target, (unsigned)a[2], -1, value);
    }
    case NR(179, 133): {
        if (a[1] != 8) return -NOS_EINVAL;
        uint64_t mask; if (user_copy_in(&mask, a[0], 8)) return -NOS_EFAULT;
        current_task->signal_restore_mask = current_task->signal_mask; current_task->signal_suspend = 1;
        current_task->signal_mask = mask & ~((1ull << 8) | (1ull << 18));
        current_task->signal_wait_set = 0; current_task->wake_tick = 0;
        if (!(current_task->signal_pending & ~current_task->signal_mask)) current_task->state = TASK_SIGNAL_WAIT;
        reschedule = 1; return -NOS_EINTR;
    }
#ifdef NOS_ARM
    case 421:
#endif
    case NR(177, 137): {
        if (a[3] != 8) return -NOS_EINVAL;
        uint64_t mask; if (user_copy_in(&mask, a[0], 8)) return -NOS_EFAULT;
        if (a[1] && !user_access(current_task, a[1], 128, 1)) return -NOS_EFAULT;
        unsigned char info[128]; int signal = dequeue(current_task, mask, info);
        if (signal) { int rc = a[1] ? user_copy_out(a[1], info, 128) : 0; return rc ? rc : signal; }
        uint64_t ticks = 0;
        if (a[2]) {
#ifdef NOS_ARM
            int64_t duration[2];
            if (number != 421) {
                int32_t narrow[2]; if (user_copy_in(narrow, a[2], sizeof(narrow))) return -NOS_EFAULT;
                duration[0] = narrow[0]; duration[1] = narrow[1];
            } else if (user_copy_in(duration, a[2], sizeof(duration))) return -NOS_EFAULT;
#else
            int64_t duration[2];
            if (user_copy_in(duration, a[2], sizeof(duration))) return -NOS_EFAULT;
#endif
            if (duration[0] < 0 || duration[1] < 0 || duration[1] >= 1000000000) return -NOS_EINVAL;
            if ((uint64_t)duration[0] > (UINT64_MAX - 100) / 100) return -NOS_EINVAL;
            ticks = (uint64_t)duration[0] * 100 + ((uint64_t)duration[1] + 9999999) / 10000000;
            if (!ticks) return -NOS_EAGAIN;
        }
        current_task->signal_wait_set = mask; current_task->signal_wait_info = a[1];
        current_task->wake_tick = ticks ? kernel_ticks + ticks : 0; current_task->state = TASK_SIGNAL_WAIT;
        reschedule = 1; return 0;
    }
    default: return NOS_LINUX_NOT_HANDLED;
    }
}
#else
void linux_wait_control_end(struct task *task) { (void)task; }
intptr_t linux_wait_control(uintptr_t operation, uintptr_t argument, uintptr_t size)
{ (void)operation; (void)argument; (void)size; return -NOS_ENOTSUP; }
void linux_signal_reset(struct task *task) { (void)task; }
void linux_signal_exec(struct task *task, unsigned old_space) { (void)task; (void)old_space; }
void linux_signal_retid(struct task *source, struct task *destination) { (void)source; (void)destination; }
void linux_signal_interrupted(struct task *task) { (void)task; }
void linux_signal_interrupt_pending(struct task *task) { (void)task; }
void linux_signal_fork(struct task *child, const struct task *parent) { (void)child; (void)parent; }
void linux_signal_discard(struct task *task) { (void)task; }
void linux_signal_deliver(struct task *task) { (void)task; }
void linux_signal_tick(void) { }
int linux_signal_send(struct task *task, unsigned signal, int code, unsigned sender, uintptr_t value)
{ (void)task; (void)signal; (void)code; (void)sender; (void)value; return -NOS_ENOTSUP; }
intptr_t linux_signal_syscall(uintptr_t number, const uintptr_t arguments[6])
{ (void)number; (void)arguments; return NOS_LINUX_NOT_HANDLED; }
#endif
