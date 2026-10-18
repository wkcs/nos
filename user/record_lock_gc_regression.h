/* Copyright (C) 2026 NOS contributors. Private to kernel_regression.c. */
#ifndef NOS_RECORD_LOCK_GC_REGRESSION_H
#define NOS_RECORD_LOCK_GC_REGRESSION_H
#if defined(NOS_MMU) && defined(NOS_LINUX_COMPAT) && NOS_PERSISTENCE_TEST == 0
#include "linux.h"
static void record_gc_churn(uintptr_t unused)
{ (void)unused; nos_syscall(NOS_SYS_EXIT, 91, 0, 0); }
static intptr_t record_gc_exec(const char *mode, const unsigned char *random)
{
    const char *path = "/bin/glibc_record_gc";
    const char *arguments[] = {path, mode, 0};
    const char *environment[] = {"LD_LIBRARY_PATH=/lib", 0};
    struct nos_exec_request request = {path, arguments, environment, 0};
    return nos_syscall(NOS_SYS_EXEC_LINUX, (uintptr_t)&request, (uintptr_t)random, NOS_EXEC_RANDOM_VALID);
}
static void record_gc_supervisor(uintptr_t unused)
{
    (void)unused;
    unsigned wanted = NOS_CAP_SPAWN | NOS_CAP_PROCESS_QUERY;
    while (((unsigned)nos_syscall(NOS_SYS_CAPS, 0, 0, 0) & wanted) != wanted)
        nos_syscall(NOS_SYS_SLEEP, 1, 0, 0);
    unsigned char random[48];
    /* Obtain independent AT_RANDOM inputs before withholding CPU from VFS. */
    for (size_t done = 0; done < sizeof(random);) {
        struct nos_message entropy = {.op = NOS_VFS_GETRANDOM, .len = sizeof(random) - done};
        intptr_t rc = nos_rpc(&entropy);
        if (rc <= 0 || (uintptr_t)rc > sizeof(random) - done || entropy.len != (size_t)rc)
            nos_syscall(NOS_SYS_EXIT, 92, 0, 0);
        u_memcpy(random + done, entropy.data, (size_t)rc); done += (size_t)rc;
    }
    intptr_t owner = record_gc_exec("owner", random);
    if (!wait_for(owner, 0) || nos_syscall(NOS_SYS_GROUP_ALIVE, (uintptr_t)owner, 0, 0))
        nos_syscall(NOS_SYS_EXIT, 93, 0, 0);
    /* No VFS RPC occurs here. Each new generation overwrites the same pending
     * exit slot, while the committed record lock belongs to the old full PID. */
    for (unsigned n = 0; n < NOS_TASKS + 1; n++) {
        intptr_t child = spawn(record_gc_churn, 0, 0);
        if (child < 0 || child == owner || (uintptr_t)child % NOS_TASKS != (uintptr_t)owner % NOS_TASKS ||
            !wait_for(child, 91) || nos_syscall(NOS_SYS_GROUP_ALIVE, (uintptr_t)owner, 0, 0))
            nos_syscall(NOS_SYS_EXIT, 94, 0, 0);
    }
    intptr_t observer = record_gc_exec("observer", random + 16);
    if (!wait_for(observer, 0)) nos_syscall(NOS_SYS_EXIT, 95, 0, 0);
    intptr_t inherited = record_gc_exec("fork-lease", random + 32);
    nos_syscall(NOS_SYS_EXIT, wait_for(inherited, 0) ? 90 : 96, 0, 0);
}
static int record_gc_reclaimed(const struct nos_object_stats *before, struct nos_object_stats *after)
{
    for (unsigned attempt = 0; attempt < 100; attempt++) {
        nos_syscall(NOS_SYS_OBJECT_STATS, (uintptr_t)after, 0, 0);
        if (before->tasks == after->tasks && before->endpoints == after->endpoints && before->free_pages == after->free_pages)
            return 1;
        nos_syscall(NOS_SYS_SLEEP, 1, 0, 0);
    }
    return 0;
}
static void record_gc_diagnostic(const char *label, const intptr_t *values, unsigned count)
{
    /* nos_puts uses direct NOS_SYS_WRITE, never VFS or stdio. */
    nos_puts(label);
    for (unsigned i = 0; i < count; i++) {
        char digits[3 * sizeof(uintptr_t) + 2]; unsigned n = sizeof(digits) - 1;
        uintptr_t value = values[i] < 0 ? (uintptr_t)(-(values[i] + 1)) + 1 : (uintptr_t)values[i];
        digits[n] = 0;
        do { digits[--n] = (char)('0' + value % 10); value /= 10; } while (value);
        if (values[i] < 0) digits[--n] = '-';
        nos_puts(" "); nos_puts(digits + n);
    }
    nos_puts("\n");
}
#if NOS_SMP == 1
/* Private fixture layout, shared with apps/glibc_record_gc.c. */
struct pipe_gc_control {
    uint32_t armed, observer_pid, observer_ready, start_observer;
    uint32_t done, write_returned, capacity, result;
};
_Static_assert(sizeof(struct pipe_gc_control) == 32, "pipe GC fixture layout");
static void record_pipe_supervisor(uintptr_t unused)
{
    (void)unused;
    while (!(nos_syscall(NOS_SYS_CAPS, 0, 0, 0) & NOS_CAP_SPAWN)) nos_syscall(NOS_SYS_SLEEP, 1, 0, 0);
    const char *path = "/tmp/pipe-gc-control";
    unsigned step = 1;
    intptr_t owner_status = -1;
    int fd = nos_open(path, NOS_O_CREAT | NOS_O_EXCL | NOS_O_RDWR);
    intptr_t owner = -1, observer = -1, mapped = -1;
    struct pipe_gc_control *control = 0;
    int ok = fd >= 0;
    if (!ok) goto cleanup;
    step = 2;
    uint64_t length = NOS_PAGE_SIZE;
    struct nos_message resize = {.op = NOS_VFS_FTRUNCATE, .arg = fd, .len = sizeof(length)};
    u_memcpy(resize.data, &length, sizeof(length));
    if (nos_rpc(&resize)) { ok = 0; goto cleanup; }
    step = 3;
    struct nos_message lease = {.op = NOS_VFS_SHARED_FILE, .arg = fd, .value = NOS_PROT_READ | NOS_PROT_WRITE};
    intptr_t handle = nos_rpc(&lease);
    if (nos_pointer_is_error(handle)) { ok = 0; goto cleanup; }
    step = 4;
    struct nos_shared_request request = {.handle = (uintptr_t)handle, .length = NOS_PAGE_SIZE,
        .prot = NOS_PROT_READ | NOS_PROT_WRITE, .flags = 1};
    mapped = nos_syscall(NOS_SYS_SHARED, NOS_SHARED_MAP, (uintptr_t)&request, 0);
    if (nos_pointer_is_error(mapped)) { ok = 0; goto cleanup; }
    control = (void *)(uintptr_t)mapped; u_memset(control, 0, sizeof(*control));
    step = 5;
    /* The three existing EXECs retain their independent 48 bytes. This
     * additional process receives a separate, freshly obtained 16 bytes. */
    unsigned char random[16];
    for (size_t done = 0; done < sizeof(random);) {
        struct nos_message entropy = {.op = NOS_VFS_GETRANDOM, .len = sizeof(random) - done};
        intptr_t rc = nos_rpc(&entropy);
        if (rc <= 0 || (uintptr_t)rc > sizeof(random) - done || entropy.len != (size_t)rc) {
            ok = 0; goto cleanup;
        }
        u_memcpy(random + done, entropy.data, (size_t)rc); done += (size_t)rc;
    }
    step = 6; owner = record_gc_exec("pipe-owner", random);
    if (owner < 0) { ok = 0; goto cleanup; }
    unsigned armed = 0;
    step = 7;
    for (unsigned attempt = 0; attempt < 1000; attempt++) {
        if (__atomic_load_n(&control->armed, __ATOMIC_ACQUIRE) &&
            __atomic_load_n(&control->observer_ready, __ATOMIC_ACQUIRE)) { armed = 1; break; }
        if (nos_syscall(NOS_SYS_WAIT, (uintptr_t)owner, (uintptr_t)&owner_status, NOS_WAIT_NONBLOCK) != -NOS_EAGAIN) break;
        nos_syscall(NOS_SYS_SLEEP, 1, 0, 0);
    }
    observer = control->observer_pid;
    /* On one CPU neither the priority-zero owner nor VFS with its priority
     * donation can yield execution to this priority-two task while runnable.
     * armed is set immediately before the full-pipe WRITE, with no intervening
     * syscall. A live owner and execution here therefore prove it is blocked,
     * and VFS has run the request through defer/RECV. No sleep duration is used
     * to infer that state; sleeps above only await the shared predicate. */
    ok = armed && observer > 0 && control->capacity && !control->write_returned &&
        nos_syscall(NOS_SYS_PRIORITY, 0, 0, 0) == 2 && nos_syscall(NOS_SYS_PRIORITY, 1, 0, 0) == 2 &&
        nos_syscall(NOS_SYS_GROUP_ALIVE, (uintptr_t)owner, 0, 0) == NOS_GROUP_LIVE_TASKS;
    if (!ok) goto cleanup;
    step = 8;
    ok = !nos_syscall(NOS_SYS_TASK_CANCEL, (uintptr_t)owner, NOS_CANCEL_GROUP, 0) &&
        !nos_syscall(NOS_SYS_WAIT, (uintptr_t)owner, (uintptr_t)&owner_status, 0) && owner_status == -NOS_EINTR;
    if (!ok) goto cleanup;
    /* Death removes the donation; VFS returns to base four. No VFS RPC or
     * sleep occurs until the former owner's exit slot has been overwritten. */
    step = 9;
    for (unsigned n = 0; n < NOS_TASKS + 1; n++) {
        intptr_t child = spawn(record_gc_churn, 0, 0);
        if (child < 0 || child == owner || (uintptr_t)child % NOS_TASKS != (uintptr_t)owner % NOS_TASKS ||
            !wait_for(child, 91)) { ok = 0; goto cleanup; }
    }
    step = 10; __atomic_store_n(&control->start_observer, 1u, __ATOMIC_RELEASE);
    unsigned done = 0;
    for (unsigned attempt = 0; attempt < 1000; attempt++) {
        if (__atomic_load_n(&control->done, __ATOMIC_ACQUIRE) &&
            !nos_syscall(NOS_SYS_GROUP_ALIVE, (uintptr_t)observer, 0, 0)) { done = 1; break; }
        nos_syscall(NOS_SYS_SLEEP, 1, 0, 0);
    }
    ok = done && !control->result && !control->write_returned;
cleanup:
    if (owner >= 0) {
        intptr_t status;
        intptr_t rc = nos_syscall(NOS_SYS_WAIT, (uintptr_t)owner, (uintptr_t)&status, NOS_WAIT_NONBLOCK);
        if (rc == -NOS_EAGAIN) {
            nos_syscall(NOS_SYS_TASK_CANCEL, (uintptr_t)owner, NOS_CANCEL_GROUP, 0);
            nos_syscall(NOS_SYS_WAIT, (uintptr_t)owner, (uintptr_t)&status, 0);
        }
    }
    if (control && observer <= 0) observer = control->observer_pid;
    if (observer > 0 && nos_syscall(NOS_SYS_GROUP_ALIVE, (uintptr_t)observer, 0, 0) > 0) {
        nos_syscall(NOS_SYS_TASK_CANCEL, (uintptr_t)observer, NOS_CANCEL_GROUP, 0);
        ok = 0;
    }
    intptr_t details[] = {step, owner, observer, owner_status,
        control ? (intptr_t)control->armed : -1, control ? (intptr_t)control->observer_ready : -1,
        control ? (intptr_t)control->write_returned : -1, control ? (intptr_t)control->capacity : -1,
        control ? (intptr_t)control->done : -1, control ? (intptr_t)control->result : -1};
    if (!nos_pointer_is_error(mapped)) ok &= !nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)mapped, NOS_PAGE_SIZE, 0);
    if (fd >= 0) ok &= !nos_close(fd);
    ok &= !nos_unlink(path);
    if (!ok) record_gc_diagnostic("pipe GC step/owner/observer/status/armed/ready/returned/capacity/done/result:",
        details, sizeof(details) / sizeof(details[0]));
    nos_syscall(NOS_SYS_EXIT, ok ? 97 : 98, 0, 0);
}
#endif
static void nos_record_gc_regression(void)
{
    /* Virtio RNG initializes a persistent three-page DMA queue on first use.
     * Start that service fixture before sampling; all process allocations
     * below must still return exactly to the resulting resource baseline. */
    struct nos_message entropy = {.op = NOS_VFS_GETRANDOM, .len = 1};
    int ready = nos_rpc(&entropy) == 1 && entropy.len == 1;
    struct nos_object_stats before, after;
    nos_syscall(NOS_SYS_OBJECT_STATS, (uintptr_t)&before, 0, 0);
    /* The single CPU run proves VFS could not consume the overwritten notices.
     * SMP runs exercise the same lifecycle without claiming that forced order. */
    intptr_t child = ready ? spawn(record_gc_supervisor, 0, 0) : -1;
    intptr_t status = -1;
    int ok = child >= 0 && !nos_syscall(NOS_SYS_TASK_AUTHORIZE, (uintptr_t)child,
        NOS_CAP_SPAWN | NOS_CAP_PROCESS_QUERY, 0) &&
        !nos_syscall(NOS_SYS_WAIT, (uintptr_t)child, (uintptr_t)&status, 0) && status == 90;
    int reclaimed = record_gc_reclaimed(&before, &after);
    ok &= reclaimed;
    if (!ok) {
        intptr_t values[] = {status, before.tasks, after.tasks, before.endpoints,
            after.endpoints, before.free_pages, after.free_pages};
        record_gc_diagnostic("record GC status/tasks/endpoints/pages before,after:", values, sizeof(values) / sizeof(values[0]));
    }
    nos_check(ok, "record_lock_exit_slot_reuse_gc");
    nos_check(ok, "record_lock_pending_fork_fd_inheritance");
#if NOS_SMP == 1
    /* This phase makes a scheduler-order proof only on the single CPU run.
     * Surviving CANCEL notifications may also clear the old pending request:
     * it validates combined lifetime recovery, not an isolated GC code path. */
    child = ok ? spawn(record_pipe_supervisor, 0, 2) : -1;
    status = -1;
    int pipe_ok = child >= 0 && !nos_syscall(NOS_SYS_TASK_AUTHORIZE, (uintptr_t)child,
        NOS_CAP_SPAWN | NOS_CAP_PROCESS_QUERY, 0) &&
        !nos_syscall(NOS_SYS_WAIT, (uintptr_t)child, (uintptr_t)&status, 0) && status == 97;
    pipe_ok &= record_gc_reclaimed(&before, &after);
    if (!pipe_ok) {
        intptr_t values[] = {status, before.tasks, after.tasks, before.endpoints,
            after.endpoints, before.free_pages, after.free_pages};
        record_gc_diagnostic("pipe GC status/tasks/endpoints/pages before,after:", values, sizeof(values) / sizeof(values[0]));
    }
    nos_check(pipe_ok, "pipe_pending_owner_exit_slot_reuse");
#endif
}
#else
static void nos_record_gc_regression(void) { }
#endif
#endif
