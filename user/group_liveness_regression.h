/* Copyright (C) 2026 NOS contributors. Private to kernel_regression.c. */
#ifndef NOS_GROUP_LIVENESS_REGRESSION_H
#define NOS_GROUP_LIVENESS_REGRESSION_H
#if defined(NOS_MMU) && defined(NOS_LINUX_COMPAT)
static intptr_t group_probe_call(uintptr_t endpoint, uintptr_t group, unsigned operation)
{
    struct nos_message message = { .op = operation, .arg = (intptr_t)group };
    struct nos_call_args call = { endpoint, (uintptr_t)&message, (uintptr_t)&message, 100 };
    intptr_t rc = nos_syscall(NOS_SYS_CALL_EX, (uintptr_t)&call, 0, 0);
    return rc ? rc : message.value;
}
static int group_probe_receive(uintptr_t endpoint, struct nos_message *message)
{
    for (unsigned attempt = 0; attempt < 100; attempt++) {
        intptr_t rc = nos_syscall(NOS_SYS_RECV, endpoint, (uintptr_t)message, 0);
        if (rc != -NOS_EACCES) return (int)rc;
        nos_syscall(NOS_SYS_SLEEP, 1, 0, 0);
    }
    return -NOS_ETIMEDOUT;
}
static int group_probe_batch(uint32_t group)
{
    struct nos_group_state entries[NOS_GROUP_QUERY_MAX];
    intptr_t expected = nos_syscall(NOS_SYS_GROUP_ALIVE, group, 0, 0);
    int ok = expected >= 0;
    for (unsigned i = 0; i < NOS_GROUP_QUERY_MAX; i++) entries[i] = (struct nos_group_state){group, -77};
    ok &= !nos_syscall(NOS_SYS_GROUP_ALIVE, (uintptr_t)entries, 1, NOS_GROUP_QUERY_BATCH) && entries[0].state == expected;
    ok &= !nos_syscall(NOS_SYS_GROUP_ALIVE, (uintptr_t)entries, NOS_GROUP_QUERY_MAX, NOS_GROUP_QUERY_BATCH);
    for (unsigned i = 0; i < NOS_GROUP_QUERY_MAX; i++) ok &= entries[i].state == expected;
    ok &= nos_syscall(NOS_SYS_GROUP_ALIVE, 0, 0, NOS_GROUP_QUERY_BATCH) == -NOS_EINVAL;
    ok &= nos_syscall(NOS_SYS_GROUP_ALIVE, 0, NOS_GROUP_QUERY_MAX + 1, NOS_GROUP_QUERY_BATCH) == -NOS_EINVAL;
    ok &= nos_syscall(NOS_SYS_GROUP_ALIVE, group, 0, NOS_GROUP_QUERY_BATCH + 1) == -NOS_EINVAL;
    ok &= nos_syscall(NOS_SYS_GROUP_ALIVE, 0, 1, NOS_GROUP_QUERY_BATCH) == -NOS_EFAULT;
    for (unsigned i = 0; i < NOS_GROUP_QUERY_MAX; i++) entries[i].state = -77;
    entries[NOS_GROUP_QUERY_MAX - 1].group = (uint32_t)INT32_MAX + 1u;
    ok &= nos_syscall(NOS_SYS_GROUP_ALIVE, (uintptr_t)entries, NOS_GROUP_QUERY_MAX, NOS_GROUP_QUERY_BATCH) == -NOS_EINVAL;
    for (unsigned i = 0; i < NOS_GROUP_QUERY_MAX; i++) ok &= entries[i].state == -77;
    unsigned char raw[sizeof(struct nos_group_state) + 1];
    struct nos_group_state one = {group, -77};
    u_memcpy(raw + 1, &one, sizeof(one));
    ok &= !nos_syscall(NOS_SYS_GROUP_ALIVE, (uintptr_t)(raw + 1), 1, NOS_GROUP_QUERY_BATCH);
    u_memcpy(&one, raw + 1, sizeof(one)); ok &= one.group == group && one.state == expected;
    intptr_t area = nos_syscall(NOS_SYS_MMAP, NOS_PROT_READ | NOS_PROT_WRITE, 2 * NOS_PAGE_SIZE, 0);
    if (nos_pointer_is_error(area)) return 0;
    one.state = -77; u_memcpy((void *)(uintptr_t)area, &one, sizeof(one));
    uint32_t *edge = (void *)((uintptr_t)area + NOS_PAGE_SIZE - sizeof(uint32_t)); *edge = group;
    ok &= !nos_syscall(NOS_SYS_MPROTECT, (uintptr_t)area + NOS_PAGE_SIZE, 0, NOS_PAGE_SIZE);
    ok &= !nos_syscall(NOS_SYS_MPROTECT, (uintptr_t)area, NOS_PROT_READ, NOS_PAGE_SIZE);
    ok &= nos_syscall(NOS_SYS_GROUP_ALIVE, (uintptr_t)area, 1, NOS_GROUP_QUERY_BATCH) == -NOS_EFAULT;
    struct nos_group_state copied; u_memcpy(&copied, (const void *)(uintptr_t)area, sizeof(copied));
    ok &= copied.group == group && copied.state == -77;
    ok &= nos_syscall(NOS_SYS_GROUP_ALIVE, (uintptr_t)edge, 1, NOS_GROUP_QUERY_BATCH) == -NOS_EFAULT && *edge == group;
    ok &= nos_syscall(NOS_SYS_GROUP_ALIVE, UINTPTR_MAX - 3, 1, NOS_GROUP_QUERY_BATCH) == -NOS_EFAULT;
    ok &= !nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)area, 2 * NOS_PAGE_SIZE, 0);
    return ok;
}
static void group_probe_observer(uintptr_t endpoint)
{
    for (;;) {
        struct nos_message message;
        if (group_probe_receive(endpoint, &message)) nos_syscall(NOS_SYS_EXIT, 84, 0, 0);
        unsigned operation = (unsigned)message.op;
        if (operation == 99) message.value = 0;
        else if (operation == 5) message.value = group_probe_batch((uint32_t)message.arg);
        else if (operation == 4) message.value = nos_syscall(NOS_SYS_TASK_AUTHORIZE,
            (uintptr_t)message.arg, NOS_CAP_PROCESS_QUERY, 0);
        else message.value = nos_syscall(NOS_SYS_GROUP_ALIVE,
            (uintptr_t)message.arg, operation == 2, operation == 3);
        if (nos_syscall(NOS_SYS_REPLY, (uintptr_t)message.sender, (uintptr_t)&message, 0))
            nos_syscall(NOS_SYS_EXIT, 85, 0, 0);
        if (operation == 99) nos_syscall(NOS_SYS_EXIT, 81, 0, 0);
    }
}
static void group_probe_thread(uintptr_t unused)
{
    (void)unused;
    for (;;) nos_syscall(NOS_SYS_SLEEP, 10000, 0, 0);
}
static void group_probe_leader(uintptr_t endpoint)
{
    struct nos_message message;
    if (group_probe_receive(endpoint, &message)) nos_syscall(NOS_SYS_EXIT, 87, 0, 0);
    intptr_t stack = nos_syscall(NOS_SYS_MMAP, NOS_PROT_READ | NOS_PROT_WRITE, 8192, 0);
    struct nos_thread_args args = { .entry = (uintptr_t)group_probe_thread,
        .stack = (uintptr_t)stack, .stack_size = 8192 };
    message.value = nos_pointer_is_error(stack) ? stack :
        nos_syscall(NOS_SYS_THREAD_CREATE, (uintptr_t)&args, UINTPTR_MAX, 0);
    intptr_t child = message.value;
    if (nos_syscall(NOS_SYS_REPLY, (uintptr_t)message.sender, (uintptr_t)&message, 0))
        nos_syscall(NOS_SYS_EXIT, 88, 0, 0);
    nos_syscall(NOS_SYS_EXIT, child < 0 ? 89 : 82, 0, 0);
}
static void group_probe_zombie(uintptr_t unused)
{ (void)unused; nos_syscall(NOS_SYS_EXIT, 83, 0, 0); }
static void group_probe_hold(uintptr_t unused)
{
    (void)unused;
    for (;;) nos_syscall(NOS_SYS_SLEEP, 10000, 0, 0);
}
static int group_probe_no_live(uintptr_t endpoint, uintptr_t group)
{
    for (unsigned attempt = 0; attempt < 100; attempt++) {
        intptr_t state = group_probe_call(endpoint, group, 1);
        if (state < 0) return 0;
        if (!(state & NOS_GROUP_LIVE_TASKS)) return state == 0;
        nos_syscall(NOS_SYS_SLEEP, 1, 0, 0);
    }
    return 0;
}
static int group_probe_live(uintptr_t endpoint, uintptr_t group)
{
    intptr_t state = group_probe_call(endpoint, group, 1);
    return state >= 0 && (state & NOS_GROUP_LIVE_TASKS);
}
static void nos_group_liveness_regression(void)
{
    struct nos_object_stats before, after;
    nos_syscall(NOS_SYS_OBJECT_STATS, (uintptr_t)&before, 0, 0);
    uintptr_t self = (uintptr_t)nos_syscall(NOS_SYS_GETPID, 0, 0, 0);
    int ok = nos_syscall(NOS_SYS_GROUP_ALIVE, self, 0, 0) == -NOS_EACCES;
    struct nos_group_state unauthorized = {(uint32_t)self, -77};
    ok &= nos_syscall(NOS_SYS_GROUP_ALIVE, (uintptr_t)&unauthorized, 1, NOS_GROUP_QUERY_BATCH) == -NOS_EACCES && unauthorized.state == -77;
    intptr_t endpoint = nos_syscall(NOS_SYS_ENDPOINT_CREATE, 0, 0, 0);
    intptr_t observer = endpoint < 0 ? endpoint : spawn(group_probe_observer, (uintptr_t)endpoint, 6);
    ok &= endpoint >= 0 && observer >= 0;
    if (endpoint < 0 || observer < 0) { nos_check(0, "group_query_capability"); return; }
    ok &= !nos_syscall(NOS_SYS_ENDPOINT_GRANT, (uintptr_t)observer, (uintptr_t)endpoint, NOS_RIGHT_RECV);
    ok &= group_probe_call((uintptr_t)endpoint, self, 1) == -NOS_EACCES;
    ok &= !nos_syscall(NOS_SYS_TASK_AUTHORIZE, (uintptr_t)observer, NOS_CAP_SPAWN, 0);
    ok &= group_probe_call((uintptr_t)endpoint, (uintptr_t)observer, 4) == -NOS_EACCES;
    /* Only the SELFTEST init can delegate this capability for the observer. */
    ok &= !nos_syscall(NOS_SYS_TASK_AUTHORIZE, (uintptr_t)observer, NOS_CAP_PROCESS_QUERY, 0);
    ok &= group_probe_live((uintptr_t)endpoint, self);
    ok &= group_probe_call((uintptr_t)endpoint, self, 2) == -NOS_EINVAL;
    ok &= group_probe_call((uintptr_t)endpoint, self, 3) == -NOS_EINVAL;
    ok &= group_probe_call((uintptr_t)endpoint, (uintptr_t)INT32_MAX + 1u, 1) == -NOS_EINVAL;
    nos_check(ok, "group_query_capability");
    nos_check(group_probe_call((uintptr_t)endpoint, self, 5) == 1, "group_query_batch_snapshot");

    intptr_t zombie = spawn(group_probe_zombie, 0, 6);
    ok = zombie >= 0 && group_probe_no_live((uintptr_t)endpoint, (uintptr_t)zombie);
    /* Keep the DEAD task unreaped while checking that a zombie is not live. */
    ok &= zombie >= 0 && group_probe_call((uintptr_t)endpoint, (uintptr_t)zombie, 1) == 0;
    ok &= wait_for(zombie, 83);
    nos_check(ok, "group_query_dead_zombie");

    intptr_t thread_endpoint = nos_syscall(NOS_SYS_ENDPOINT_CREATE, 0, 0, 0);
    intptr_t leader = thread_endpoint < 0 ? thread_endpoint : spawn(group_probe_leader, (uintptr_t)thread_endpoint, 6);
    ok = thread_endpoint >= 0 && leader >= 0;
    intptr_t thread = -1;
    if (ok) {
        ok &= !nos_syscall(NOS_SYS_ENDPOINT_GRANT, (uintptr_t)leader, (uintptr_t)thread_endpoint, NOS_RIGHT_RECV);
        thread = group_probe_call((uintptr_t)thread_endpoint, 0, 1);
        ok &= thread >= 0 && wait_for(leader, 82);
        ok &= leader != zombie && (uintptr_t)leader % NOS_TASKS == (uintptr_t)zombie % NOS_TASKS;
        ok &= group_probe_call((uintptr_t)endpoint, (uintptr_t)zombie, 1) == 0;
        ok &= group_probe_live((uintptr_t)endpoint, (uintptr_t)leader);
        ok &= group_probe_call((uintptr_t)endpoint, (uintptr_t)thread, 1) == 0;
    }
    nos_check(ok, "group_query_leader_exit_thread_live");

    intptr_t replacement = spawn(group_probe_hold, 0, 6);
    ok = ok && replacement >= 0 && replacement != leader &&
        (uintptr_t)replacement % NOS_TASKS == (uintptr_t)leader % NOS_TASKS;
    if (replacement >= 0) {
        ok &= group_probe_live((uintptr_t)endpoint, (uintptr_t)replacement);
        ok &= group_probe_live((uintptr_t)endpoint, (uintptr_t)leader);
    }
    if (thread >= 0) {
        ok &= !nos_syscall(NOS_SYS_TASK_CANCEL, (uintptr_t)thread, 0, 0);
        ok &= group_probe_no_live((uintptr_t)endpoint, (uintptr_t)leader);
    }
    if (replacement >= 0) {
        ok &= group_probe_live((uintptr_t)endpoint, (uintptr_t)replacement);
        ok &= !nos_syscall(NOS_SYS_TASK_CANCEL, (uintptr_t)replacement, 0, 0);
        ok &= wait_for(replacement, -NOS_EINTR);
        ok &= group_probe_no_live((uintptr_t)endpoint, (uintptr_t)replacement);
    }
    nos_check(ok, "group_query_generation_slot_reuse");
    ok = !group_probe_call((uintptr_t)endpoint, 0, 99) && wait_for(observer, 81);
    nos_syscall(NOS_SYS_OBJECT_STATS, (uintptr_t)&after, 0, 0);
    ok &= before.tasks == after.tasks && before.endpoints == after.endpoints && before.free_pages == after.free_pages;
    nos_check(ok, "group_query_observer_cleanup");
}
#else
static void nos_group_liveness_regression(void) { }
#endif
#endif
