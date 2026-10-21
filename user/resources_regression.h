/* Copyright (C) 2026 NOS contributors. Private to kernel_regression.c. */
#ifndef NOS_RESOURCES_REGRESSION_H
#define NOS_RESOURCES_REGRESSION_H
#if defined(NOS_MMU) && defined(NOS_LINUX_COMPAT)
static int resource_fill_is(const void *pointer, size_t size, unsigned char value)
{
    const unsigned char *bytes = pointer;
    for (size_t i = 0; i < size; i++) if (bytes[i] != value) return 0;
    return 1;
}
static int resource_equal(const void *left, const void *right, size_t size)
{
    const unsigned char *a = left, *b = right;
    for (size_t i = 0; i < size; i++) if (a[i] != b[i]) return 0;
    return 1;
}
static intptr_t resource_query(const struct nos_message *message, struct nos_ipc_limits *limits)
{
    struct nos_ipc_subject_query query = { .sender = (uint32_t)message->sender,
        .token = message->token, .output = (uintptr_t)limits };
    return nos_syscall(NOS_SYS_IPC_LIMITS, (uintptr_t)&query, 0, 0);
}
static int resource_denied(struct nos_ipc_subject_query *query, intptr_t expected)
{
    struct nos_ipc_limits limits;
    u_memset(&limits, 0xa7, sizeof(limits)); query->output = (uintptr_t)&limits;
    return nos_syscall(NOS_SYS_IPC_LIMITS, (uintptr_t)query, 0, 0) == expected &&
        resource_fill_is(&limits, sizeof(limits), 0xa7);
}
static intptr_t resource_call(uintptr_t endpoint, struct nos_message *message)
{
    struct nos_call_args args = { endpoint, (uintptr_t)message, (uintptr_t)message, 1000 };
    return nos_syscall(NOS_SYS_CALL_EX, (uintptr_t)&args, 0, 0);
}
static int resource_receive(uintptr_t endpoint, struct nos_message *message)
{
    for (unsigned i = 0; i < 1000; i++) {
        intptr_t rc = nos_syscall(NOS_SYS_RECV, endpoint, (uintptr_t)message, 0);
        if (rc == -NOS_EACCES) { nos_syscall(NOS_SYS_SLEEP, 1, 0, 0); continue; }
        if (rc) return (int)rc;
        if (message->op >= 0) return 0;
    }
    return -NOS_ETIMEDOUT;
}
static void resource_client(uintptr_t endpoint)
{
    struct nos_message message = { .op = 1 };
    intptr_t rc = resource_call(endpoint, &message);
    nos_syscall(NOS_SYS_EXIT, !rc && message.value == 1 ? 111 : 112, 0, 0);
}
static void resource_canceled_client(uintptr_t endpoint)
{
    struct nos_message message = { .op = 2 };
    intptr_t rc = resource_call(endpoint, &message);
    nos_syscall(NOS_SYS_EXIT, rc == -NOS_EINTR ? 113 : 114, 0, 0);
}
static void resource_wrong_server(uintptr_t endpoint)
{
    struct nos_message message;
    if (resource_receive(endpoint, &message)) nos_syscall(NOS_SYS_EXIT, 116, 0, 0);
    struct nos_ipc_subject_query query = { .sender = (uint32_t)message.value };
    u_memcpy(&query.token, message.data, sizeof(query.token));
    message.value = resource_denied(&query, -NOS_EPERM);
    int ok = !nos_syscall(NOS_SYS_REPLY, (uintptr_t)message.sender, (uintptr_t)&message, 0);
    nos_syscall(NOS_SYS_EXIT, ok ? 115 : 116, 0, 0);
}
static int resource_preflight(const struct nos_message *message)
{
    struct nos_ipc_subject_query query = { .sender = (uint32_t)message->sender, .token = message->token };
    struct nos_ipc_limits limits;
    int ok = !resource_query(message, &limits);
    query.token++; ok &= resource_denied(&query, -NOS_EPERM); query.token--;
    query.sender += NOS_TASKS; ok &= resource_denied(&query, -NOS_EPERM); query.sender -= NOS_TASKS;
    query.reserved = 1; ok &= resource_denied(&query, -NOS_EINVAL); query.reserved = 0;
    ok &= nos_syscall(NOS_SYS_IPC_LIMITS, 1, 0, 0) == -NOS_EFAULT;
    query.output = 0;
    ok &= nos_syscall(NOS_SYS_IPC_LIMITS, (uintptr_t)&query, 0, 0) == -NOS_EFAULT;
    query.output = UINT64_MAX - 10;
    ok &= nos_syscall(NOS_SYS_IPC_LIMITS, (uintptr_t)&query, 0, 0) == -NOS_EFAULT;
#ifdef NOS_ARM
    query.output = (1ull << 32) | (uintptr_t)&limits;
    ok &= nos_syscall(NOS_SYS_IPC_LIMITS, (uintptr_t)&query, 0, 0) == -NOS_EFAULT;
#endif
    unsigned char raw[sizeof(limits) + 2], unaligned[sizeof(query) + 1];
    u_memset(raw, 0x6d, sizeof(raw)); query.output = (uintptr_t)(raw + 1);
    u_memcpy(unaligned + 1, &query, sizeof(query));
    ok &= !nos_syscall(NOS_SYS_IPC_LIMITS, (uintptr_t)(unaligned + 1), 0, 0) &&
        resource_equal(raw + 1, &limits, sizeof(limits)) && raw[0] == 0x6d && raw[sizeof(raw) - 1] == 0x6d;
    intptr_t area = nos_syscall(NOS_SYS_MMAP, 3, NOS_PAGE_SIZE * 2, 0);
    if (nos_pointer_is_error(area)) return 0;
    unsigned char *edge = (void *)((uintptr_t)area + NOS_PAGE_SIZE - sizeof(limits) / 2);
    u_memset((void *)(uintptr_t)area, 0x4b, NOS_PAGE_SIZE * 2);
    ok &= !nos_syscall(NOS_SYS_MPROTECT, (uintptr_t)area + NOS_PAGE_SIZE, 0, NOS_PAGE_SIZE);
    query.output = (uintptr_t)edge;
    ok &= nos_syscall(NOS_SYS_IPC_LIMITS, (uintptr_t)&query, 0, 0) == -NOS_EFAULT &&
        resource_fill_is(edge, sizeof(limits) / 2, 0x4b);
    query.output = (uintptr_t)area;
    ok &= !nos_syscall(NOS_SYS_MPROTECT, (uintptr_t)area, NOS_PROT_READ, NOS_PAGE_SIZE);
    ok &= nos_syscall(NOS_SYS_IPC_LIMITS, (uintptr_t)&query, 0, 0) == -NOS_EFAULT &&
        resource_fill_is((void *)(uintptr_t)area, sizeof(limits), 0x4b);
    ok &= !nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)area, NOS_PAGE_SIZE * 2, 0);
    return ok;
}
static void nos_resources_regression(void)
{
    struct nos_object_stats before, after;
    nos_syscall(NOS_SYS_OBJECT_STATS, (uintptr_t)&before, 0, 0);
    intptr_t endpoint = nos_syscall(NOS_SYS_ENDPOINT_CREATE, 0, 0, 0);
    intptr_t client = endpoint < 0 ? endpoint : spawn(resource_client, (uintptr_t)endpoint, 6);
    if (client < 0) {
        if (endpoint >= 0) nos_syscall(NOS_SYS_ENDPOINT_CLOSE, (uintptr_t)endpoint, 0, 0);
        nos_check(0, "ipc_limits_authority_preflight"); return;
    }
    struct nos_message message;
    int ok = !resource_receive((uintptr_t)endpoint, &message) && message.op == 1;
    struct nos_ipc_limits limits;
    ok &= !resource_query(&message, &limits) && limits.sender == (uint32_t)client &&
        limits.process == (uint32_t)client && limits.token == message.token &&
        limits.nofile_soft == NOS_FD_LIMIT && limits.nofile_hard == NOS_FD_LIMIT;
    nos_check(ok && resource_preflight(&message), "ipc_limits_authority_preflight");
    intptr_t other_endpoint = nos_syscall(NOS_SYS_ENDPOINT_CREATE, 0, 0, 0);
    intptr_t other = other_endpoint < 0 ? other_endpoint : spawn(resource_wrong_server, (uintptr_t)other_endpoint, 6);
    int authority = other >= 0 && !nos_syscall(NOS_SYS_ENDPOINT_GRANT, (uintptr_t)other, (uintptr_t)other_endpoint, NOS_RIGHT_RECV);
    struct nos_message probe = { .op = 1, .value = message.sender, .len = sizeof(message.token) };
    u_memcpy(probe.data, &message.token, sizeof(message.token));
    if (other >= 0) {
        intptr_t rc = resource_call((uintptr_t)other_endpoint, &probe);
        if (rc) nos_syscall(NOS_SYS_TASK_CANCEL, (uintptr_t)other, 0, 0);
        int reaped = wait_for(other, 115);
        authority &= !rc && probe.value == 1 && reaped;
    }
    if (other_endpoint >= 0) nos_syscall(NOS_SYS_ENDPOINT_CLOSE, (uintptr_t)other_endpoint, 0, 0);
    nos_check(authority, "ipc_limits_server_authority");
    struct nos_ipc_subject_query old = { .sender = (uint32_t)message.sender, .token = message.token };
    message.value = 1;
    ok &= !nos_syscall(NOS_SYS_REPLY, (uintptr_t)message.sender, (uintptr_t)&message, 0) &&
        resource_denied(&old, -NOS_EPERM) && wait_for(client, 111);
    client = spawn(resource_canceled_client, (uintptr_t)endpoint, 6);
    ok &= client >= 0 && !resource_receive((uintptr_t)endpoint, &message) && message.op == 2;
    ok &= !resource_query(&message, &limits);
    old.sender = (uint32_t)message.sender; old.token = message.token;
    ok &= !nos_syscall(NOS_SYS_IPC_CANCEL, (uintptr_t)client, 0, 0) &&
        resource_denied(&old, -NOS_EPERM) && wait_for(client, 113);
    nos_check(ok, "ipc_limits_replied_and_canceled_lifetime");
    ok = !nos_syscall(NOS_SYS_ENDPOINT_CLOSE, (uintptr_t)endpoint, 0, 0);
    nos_syscall(NOS_SYS_OBJECT_STATS, (uintptr_t)&after, 0, 0);
    nos_check(ok && before.tasks == after.tasks && before.endpoints == after.endpoints &&
        before.free_pages == after.free_pages, "ipc_limits_native_cleanup");
}
#else
static void nos_resources_regression(void) { }
#endif
#endif
