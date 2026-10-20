/* Copyright (C) 2026 NOS contributors. Private to kernel_regression.c. */
#ifndef NOS_CREDENTIALS_REGRESSION_H
#define NOS_CREDENTIALS_REGRESSION_H
#if defined(NOS_MMU) && defined(NOS_LINUX_COMPAT)
static int credential_bytes(const void *left, const void *right, size_t size)
{
    const unsigned char *a = left, *b = right;
    for (size_t i = 0; i < size; i++) if (a[i] != b[i]) return 0;
    return 1;
}
static int credential_fill_is(const void *pointer, size_t size, unsigned char value)
{
    const unsigned char *bytes = pointer;
    for (size_t i = 0; i < size; i++) if (bytes[i] != value) return 0;
    return 1;
}
static intptr_t credential_get(struct nos_credentials *value)
{ return nos_syscall(NOS_SYS_CREDENTIALS, NOS_CRED_GET, (uintptr_t)value, 0); }
static intptr_t credential_set(unsigned command, uint32_t r, uint32_t e, uint32_t s)
{
    struct nos_credential_request request = {.ids = {r, e, s}};
    return nos_syscall(NOS_SYS_CREDENTIALS, command, (uintptr_t)&request, 0);
}
static int credential_identity(unsigned phase)
{
    struct nos_credential_request groups = {.count = phase ? 3 : 2,
        .groups = {phase ? 91 : 71, phase ? 92 : 72, 93}};
    return !credential_set(NOS_CRED_SETEUID, 0, 0, 0) &&
        !nos_syscall(NOS_SYS_CREDENTIALS, NOS_CRED_SETGROUPS, (uintptr_t)&groups, 0) &&
        !credential_set(NOS_CRED_SETRESGID, 0, phase ? 70003 : 70002, 0) &&
        !credential_set(NOS_CRED_SETRESUID, 0, phase ? 70004 : 70001, 0);
}
static int credential_expected(const struct nos_credentials *value, unsigned phase)
{
    return !value->ruid && value->euid == (phase ? 70004u : 70001u) && !value->suid &&
        !value->rgid && value->egid == (phase ? 70003u : 70002u) && !value->sgid &&
        value->ngroups == (phase ? 3u : 2u) && !value->reserved &&
        value->groups[0] == (phase ? 91u : 71u) && value->groups[1] == (phase ? 92u : 72u) &&
        value->groups[2] == (phase ? 93u : 0u);
}
static intptr_t credential_subject(const struct nos_message *message, struct nos_ipc_subject *subject)
{
    struct nos_ipc_subject_query query = {.sender = (uint32_t)message->sender,
        .token = message->token, .output = (uintptr_t)subject};
    return nos_syscall(NOS_SYS_IPC_SUBJECT, (uintptr_t)&query, 0, 0);
}
static int credential_denied(struct nos_ipc_subject_query *query, intptr_t expected)
{
    struct nos_ipc_subject subject;
    u_memset(&subject, 0xa7, sizeof(subject)); query->output = (uintptr_t)&subject;
    return nos_syscall(NOS_SYS_IPC_SUBJECT, (uintptr_t)query, 0, 0) == expected &&
        credential_fill_is(&subject, sizeof(subject), 0xa7);
}
static int credential_receive(uintptr_t endpoint, struct nos_message *message)
{
    unsigned grant_waits = 0;
    for (;;) {
        intptr_t rc = nos_syscall(NOS_SYS_RECV, endpoint, (uintptr_t)message, 0);
        /* A spawned server may run before the parent transfers RECV. */
        if (rc == -NOS_EACCES && grant_waits++ < 1000) {
            nos_syscall(NOS_SYS_SLEEP, 1, 0, 0);
            continue;
        }
        if (rc) return (int)rc;
        if (message->op >= 0) return 0;
    }
}
static intptr_t credential_call(uintptr_t endpoint, struct nos_message *message, unsigned timeout)
{
    struct nos_call_args args = {endpoint, (uintptr_t)message, (uintptr_t)message, timeout};
    return nos_syscall(NOS_SYS_CALL_EX, (uintptr_t)&args, 0, 0);
}
struct credential_change_control { unsigned gate, done, tid; uintptr_t endpoint; };
static void credential_change_thread(uintptr_t pointer)
{
    struct credential_change_control *control = (void *)pointer;
    for (unsigned i = 0; !__atomic_load_n(&control->gate, __ATOMIC_ACQUIRE) && i < 1000; i++)
        nos_syscall(NOS_SYS_SLEEP, 1, 0, 0);
    struct nos_credentials current;
    int ok = __atomic_load_n(&control->gate, __ATOMIC_ACQUIRE) &&
        !credential_get(&current) && credential_expected(&current, 0) && credential_identity(1);
    __atomic_store_n(&control->done, ok ? 1u : 2u, __ATOMIC_RELEASE);
    nos_syscall(NOS_SYS_EXIT, 0, 0, 0);
}
static void credential_snapshot_client(uintptr_t endpoint)
{
    intptr_t stack = nos_syscall(NOS_SYS_MMAP, 3, 8192, 0);
    struct credential_change_control control = {.endpoint = endpoint};
    struct nos_thread_args args = {.entry = (uintptr_t)credential_change_thread,
        .arg = (uintptr_t)&control, .stack = (uintptr_t)stack, .stack_size = 8192,
        .clear_tid = (uintptr_t)&control.tid, .parent_tid = (uintptr_t)&control.tid};
    int ok = !nos_pointer_is_error(stack) && credential_identity(0);
    intptr_t worker = ok ? nos_syscall(NOS_SYS_THREAD_CREATE, (uintptr_t)&args, UINTPTR_MAX, 0) : -1;
    ok &= worker >= 0;
    struct nos_message message = {.op = 1, .bulk_in = (uintptr_t)&control,
        .bulk_out = (uintptr_t)&control, .bulk_in_len = sizeof(control), .bulk_out_len = sizeof(control)};
    ok &= !credential_call(endpoint, &message, 1000) && message.value == 1;
    struct nos_credentials current;
    ok &= !credential_get(&current) && credential_expected(&current, 1);
    message = (struct nos_message){.op = 2, .value = ok};
    ok &= !credential_call(endpoint, &message, 1000) && message.value == 1;
    unsigned tid;
    while ((tid = __atomic_load_n(&control.tid, __ATOMIC_ACQUIRE)))
        nos_syscall(NOS_SYS_FUTEX_WAIT, (uintptr_t)&control.tid, tid, 0);
    if (!nos_pointer_is_error(stack)) ok &= !nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)stack, 8192, 0);
    nos_syscall(NOS_SYS_EXIT, ok ? 91 : 92, 0, 0);
}
static int credential_query_preflight(const struct nos_message *message)
{
    struct nos_ipc_subject_query query = {.sender = (uint32_t)message->sender, .token = message->token};
    struct nos_ipc_subject subject;
    int ok = !credential_subject(message, &subject);
    query.token++;
    ok &= credential_denied(&query, -NOS_EPERM); query.token--;
    query.sender += NOS_TASKS;
    ok &= credential_denied(&query, -NOS_EPERM); query.sender -= NOS_TASKS;
    query.reserved = 1;
    ok &= credential_denied(&query, -NOS_EINVAL); query.reserved = 0;
    ok &= nos_syscall(NOS_SYS_IPC_SUBJECT, 1, 0, 0) == -NOS_EFAULT;
    query.output = 0;
    ok &= nos_syscall(NOS_SYS_IPC_SUBJECT, (uintptr_t)&query, 0, 0) == -NOS_EFAULT;
    query.output = UINT64_MAX - 10;
    ok &= nos_syscall(NOS_SYS_IPC_SUBJECT, (uintptr_t)&query, 0, 0) == -NOS_EFAULT;
    unsigned char raw[sizeof(struct nos_ipc_subject) + 2];
    u_memset(raw, 0x6d, sizeof(raw)); query.output = (uintptr_t)(raw + 1);
    unsigned char unaligned[sizeof(query) + 1]; u_memcpy(unaligned + 1, &query, sizeof(query));
    ok &= !nos_syscall(NOS_SYS_IPC_SUBJECT, (uintptr_t)(unaligned + 1), 0, 0) &&
        credential_bytes(raw + 1, &subject, sizeof(subject)) && raw[0] == 0x6d && raw[sizeof(raw) - 1] == 0x6d;
    intptr_t area = nos_syscall(NOS_SYS_MMAP, 3, NOS_PAGE_SIZE * 2, 0);
    if (nos_pointer_is_error(area)) return 0;
    unsigned char *edge = (void *)((uintptr_t)area + NOS_PAGE_SIZE - sizeof(subject) / 2);
    u_memset((void *)(uintptr_t)area, 0x4b, NOS_PAGE_SIZE * 2);
    ok &= !nos_syscall(NOS_SYS_MPROTECT, (uintptr_t)area + NOS_PAGE_SIZE, 0, NOS_PAGE_SIZE);
    query.output = (uintptr_t)edge;
    ok &= nos_syscall(NOS_SYS_IPC_SUBJECT, (uintptr_t)&query, 0, 0) == -NOS_EFAULT &&
        credential_fill_is(edge, sizeof(subject) / 2, 0x4b);
    query.output = (uintptr_t)area;
    ok &= !nos_syscall(NOS_SYS_MPROTECT, (uintptr_t)area, NOS_PROT_READ, NOS_PAGE_SIZE);
    ok &= nos_syscall(NOS_SYS_IPC_SUBJECT, (uintptr_t)&query, 0, 0) == -NOS_EFAULT &&
        credential_fill_is((void *)(uintptr_t)area, sizeof(subject), 0x4b);
    ok &= !nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)area, NOS_PAGE_SIZE * 2, 0);
    return ok;
}
static int credential_native_atomicity(const struct nos_credentials *initial)
{
    struct nos_credentials current;
    struct nos_credential_request request = {.count = NOS_NGROUPS_MAX + 1};
    int ok = nos_syscall(NOS_SYS_CREDENTIALS, NOS_CRED_SETGROUPS, (uintptr_t)&request, 0) == -NOS_EINVAL &&
        credential_set(NOS_CRED_SETUID, UINT32_MAX, 0, 0) == -NOS_EINVAL &&
        nos_syscall(NOS_SYS_CREDENTIALS, NOS_CRED_SETGROUPS + 1, (uintptr_t)&request, 0) == -NOS_EINVAL &&
        nos_syscall(NOS_SYS_CREDENTIALS, NOS_CRED_SETGROUPS, (uintptr_t)&request, 1) == -NOS_EINVAL;
    unsigned char raw[sizeof(current) + 2];
    u_memset(raw, 0x5c, sizeof(raw));
    ok &= !nos_syscall(NOS_SYS_CREDENTIALS, NOS_CRED_GET, (uintptr_t)(raw + 1), 0) &&
        credential_bytes(raw + 1, initial, sizeof(current)) && raw[0] == 0x5c && raw[sizeof(raw) - 1] == 0x5c;
    intptr_t area = nos_syscall(NOS_SYS_MMAP, 3, NOS_PAGE_SIZE * 2, 0);
    if (nos_pointer_is_error(area)) return 0;
    u_memset((void *)(uintptr_t)area, 0x8a, NOS_PAGE_SIZE * 2);
    unsigned char *edge = (void *)((uintptr_t)area + NOS_PAGE_SIZE - sizeof(current) / 2);
    ok &= !nos_syscall(NOS_SYS_MPROTECT, (uintptr_t)area + NOS_PAGE_SIZE, 0, NOS_PAGE_SIZE);
    ok &= credential_get((void *)edge) == -NOS_EFAULT && credential_fill_is(edge, sizeof(current) / 2, 0x8a);
    ok &= nos_syscall(NOS_SYS_CREDENTIALS, NOS_CRED_SETRESUID, (uintptr_t)edge, 0) == -NOS_EFAULT;
    ok &= !nos_syscall(NOS_SYS_MPROTECT, (uintptr_t)area, NOS_PROT_READ, NOS_PAGE_SIZE);
    ok &= credential_get((void *)(uintptr_t)area) == -NOS_EFAULT &&
        credential_fill_is((void *)(uintptr_t)area, sizeof(current), 0x8a);
    ok &= !nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)area, NOS_PAGE_SIZE * 2, 0);
    ok &= !credential_get(&current) && credential_bytes(&current, initial, sizeof(current));
    return ok;
}
static void credential_expiring_client(uintptr_t endpoint, unsigned mode)
{
    struct nos_message message = {.op = 3};
    intptr_t rc = credential_call(endpoint, &message, mode == 0 ? 20 : 1000);
    intptr_t expected = mode == 0 ? -NOS_ETIMEDOUT : mode == 1 ? -NOS_EINTR : mode == 2 ? -NOS_EACCES : -NOS_ESRCH;
    nos_syscall(NOS_SYS_EXIT, rc == expected ? 93 : 94, 0, 0);
}
/* Endpoint handles are opaque and can use every bit of a native word. Carry
 * the scenario in the entry point, leaving the full handle in the argument. */
static void credential_timeout_client(uintptr_t endpoint) { credential_expiring_client(endpoint, 0); }
static void credential_canceled_client(uintptr_t endpoint) { credential_expiring_client(endpoint, 1); }
static void credential_revoked_client(uintptr_t endpoint) { credential_expiring_client(endpoint, 2); }
static void credential_closed_client(uintptr_t endpoint) { credential_expiring_client(endpoint, 3); }
static void credential_wrong_server(uintptr_t endpoint)
{
    struct nos_message message;
    if (credential_receive(endpoint, &message)) nos_syscall(NOS_SYS_EXIT, 96, 0, 0);
    struct nos_ipc_subject_query query = {.sender = (uint32_t)message.value};
    u_memcpy(&query.token, message.data, sizeof(query.token));
    message.value = credential_denied(&query, -NOS_EPERM);
    int ok = !nos_syscall(NOS_SYS_REPLY, (uintptr_t)message.sender, (uintptr_t)&message, 0);
    nos_syscall(NOS_SYS_EXIT, ok ? 95 : 96, 0, 0);
}
static void credential_retained_thread(uintptr_t endpoint)
{
    struct nos_credentials current;
    int ok = !credential_get(&current) && credential_expected(&current, 0);
    struct nos_message message = {.op = 4, .value = ok};
    ok &= !credential_call(endpoint, &message, 1000) && message.value == 1;
    ok &= !credential_get(&current) && credential_expected(&current, 0);
    message = (struct nos_message){.op = 5, .value = ok};
    ok &= !credential_call(endpoint, &message, 1000) && message.value == 1;
    nos_syscall(NOS_SYS_EXIT, ok ? 97 : 98, 0, 0);
}
static void credential_retained_leader(uintptr_t endpoint)
{
    intptr_t stack = nos_syscall(NOS_SYS_MMAP, 3, 8192, 0);
    struct nos_thread_args args = {.entry = (uintptr_t)credential_retained_thread,
        .arg = endpoint, .stack = (uintptr_t)stack, .stack_size = 8192};
    int ok = !nos_pointer_is_error(stack) && credential_identity(0) &&
        nos_syscall(NOS_SYS_THREAD_CREATE, (uintptr_t)&args, UINTPTR_MAX, 0) >= 0;
    nos_syscall(NOS_SYS_EXIT, ok ? 99 : 100, 0, 0);
}
static void credential_replacement(uintptr_t endpoint)
{
    int ok = credential_identity(1);
    struct nos_message message = {.op = 6, .value = ok};
    ok &= !credential_call(endpoint, &message, 1000) && message.value == 1;
    nos_syscall(NOS_SYS_EXIT, ok ? 101 : 102, 0, 0);
}
static void nos_credentials_regression(void)
{
    struct nos_object_stats before, after;
    struct nos_credentials initial, current;
    nos_syscall(NOS_SYS_OBJECT_STATS, (uintptr_t)&before, 0, 0);
    int ok = !credential_get(&initial) && !initial.ruid && !initial.euid;
    nos_check(ok && credential_native_atomicity(&initial), "credentials_native_atomicity");
    intptr_t endpoint = nos_syscall(NOS_SYS_ENDPOINT_CREATE, 0, 0, 0);
    intptr_t client = endpoint < 0 ? endpoint : spawn(credential_snapshot_client, (uintptr_t)endpoint, 6);
    if (endpoint < 0 || client < 0) { nos_check(0, "ipc_subject_call_snapshot"); return; }
    struct nos_ipc_subject_query unused = {.sender = (uint32_t)client, .token = UINT64_MAX};
    ok &= credential_denied(&unused, -NOS_EPERM);
    struct nos_message message;
    ok &= !credential_receive((uintptr_t)endpoint, &message) && message.op == 1;
    struct nos_ipc_subject first, second;
    ok &= !credential_subject(&message, &first) && first.sender == (uint32_t)client &&
        first.process == (uint32_t)client && first.token == message.token && credential_expected(&first.credentials, 0);
    nos_check(credential_query_preflight(&message), "ipc_subject_authority_preflight");
    intptr_t other_endpoint = nos_syscall(NOS_SYS_ENDPOINT_CREATE, 0, 0, 0);
    intptr_t other = other_endpoint < 0 ? other_endpoint : spawn(credential_wrong_server, (uintptr_t)other_endpoint, 6);
    int authority = other >= 0 && !nos_syscall(NOS_SYS_ENDPOINT_GRANT, (uintptr_t)other, (uintptr_t)other_endpoint, NOS_RIGHT_RECV);
    struct nos_message probe = {.op = 1, .value = message.sender, .len = sizeof(message.token)};
    u_memcpy(probe.data, &message.token, sizeof(message.token));
    if (other >= 0) {
        intptr_t call_rc = credential_call((uintptr_t)other_endpoint, &probe, 1000);
        /* Reap the actor even when its RPC failed, so a failed assertion cannot
         * leave a zombie that contaminates the resource-baseline check. */
        if (call_rc) nos_syscall(NOS_SYS_TASK_CANCEL, (uintptr_t)other, 0, 0);
        intptr_t status = 0;
        intptr_t wait_rc = nos_syscall(NOS_SYS_WAIT, (uintptr_t)other, (uintptr_t)&status, 0);
        authority &= !call_rc && probe.value == 1 && !wait_rc && status == 95;
    }
    /* The server normally closes its endpoint on exit. Close any parent-owned
     * handle left by a failed grant or spawn as well. */
    if (other_endpoint >= 0) nos_syscall(NOS_SYS_ENDPOINT_CLOSE, (uintptr_t)other_endpoint, 0, 0);
    /* Tokens are uint64_t even on ARM; pass the complete token inline. */
    nos_check(authority, "ipc_subject_server_authority");
    unsigned gate = 1, done = 0;
    struct nos_ipc_xfer transfer = {message.sender, message.token, 0, (uintptr_t)&gate, sizeof(gate), NOS_IPC_STORE};
    ok &= !nos_syscall(NOS_SYS_IPC_XFER, (uintptr_t)&transfer, 0, 0);
    transfer.offset = sizeof(unsigned); transfer.buffer = (uintptr_t)&done; transfer.direction = NOS_IPC_FETCH;
    for (unsigned attempt = 0; !done && attempt < 1000; attempt++) {
        if (nos_syscall(NOS_SYS_IPC_XFER, (uintptr_t)&transfer, 0, 0)) { ok = 0; break; }
        if (!done) nos_syscall(NOS_SYS_SLEEP, 1, 0, 0);
    }
    ok &= done == 1 && !credential_subject(&message, &second) && credential_bytes(&first, &second, sizeof(first));
    message.value = ok;
    ok &= !nos_syscall(NOS_SYS_REPLY, (uintptr_t)message.sender, (uintptr_t)&message, 0);
    struct nos_ipc_subject_query old = {.sender = first.sender, .token = first.token};
    ok &= credential_denied(&old, -NOS_EPERM);
    ok &= !credential_receive((uintptr_t)endpoint, &message) && message.op == 2 && message.value == 1;
    ok &= !credential_subject(&message, &second) && credential_expected(&second.credentials, 1) && second.token != first.token;
    message.value = ok;
    ok &= !nos_syscall(NOS_SYS_REPLY, (uintptr_t)message.sender, (uintptr_t)&message, 0) && wait_for(client, 91);
    ok &= !credential_get(&current) && credential_bytes(&initial, &current, sizeof(initial));
    nos_check(ok, "ipc_subject_call_snapshot");

    ok = 1;
    void (*const expiring_clients[])(uintptr_t) = {
        credential_timeout_client, credential_canceled_client,
        credential_revoked_client, credential_closed_client
    };
    for (unsigned mode = 0; mode < 4; mode++) {
        intptr_t expiring = spawn(expiring_clients[mode], (uintptr_t)endpoint, 6);
        ok &= expiring >= 0 && !credential_receive((uintptr_t)endpoint, &message);
        ok &= !credential_subject(&message, &second);
        if (!mode) ok &= expiring != client && (uintptr_t)expiring % NOS_TASKS == (uintptr_t)client % NOS_TASKS &&
            credential_denied(&old, -NOS_EPERM);
        struct nos_ipc_subject_query expired = {.sender = (uint32_t)message.sender, .token = message.token};
        if (!mode) nos_syscall(NOS_SYS_SLEEP, 22, 0, 0);
        else if (mode == 1) ok &= !nos_syscall(NOS_SYS_IPC_CANCEL, (uintptr_t)expiring, 0, 0);
        else if (mode == 2) ok &= !nos_syscall(NOS_SYS_ENDPOINT_REVOKE,
            (uintptr_t)nos_syscall(NOS_SYS_GETPID, 0, 0, 0), (uintptr_t)endpoint, NOS_RIGHT_RECV);
        else ok &= !nos_syscall(NOS_SYS_ENDPOINT_CLOSE, (uintptr_t)endpoint, 0, 0);
        ok &= credential_denied(&expired, -NOS_EPERM) && wait_for(expiring, 93);
        if (mode == 2) ok &= !nos_syscall(NOS_SYS_ENDPOINT_GRANT,
            (uintptr_t)nos_syscall(NOS_SYS_GETPID, 0, 0, 0), (uintptr_t)endpoint, NOS_RIGHT_RECV);
    }
    nos_check(ok, "ipc_subject_revoked_lifetime");

    endpoint = nos_syscall(NOS_SYS_ENDPOINT_CREATE, 0, 0, 0);
    intptr_t leader = endpoint < 0 ? endpoint : spawn(credential_retained_leader, (uintptr_t)endpoint, 6);
    ok = leader >= 0 && !credential_receive((uintptr_t)endpoint, &message) && message.op == 4 && message.value == 1;
    struct nos_message retained = message;
    ok &= !credential_subject(&retained, &first) && first.process == (uint32_t)leader && credential_expected(&first.credentials, 0);
    ok &= wait_for(leader, 99);
    intptr_t replacement = spawn(credential_replacement, (uintptr_t)endpoint, 6);
    ok &= replacement >= 0 && replacement != leader && (uintptr_t)replacement % NOS_TASKS == (uintptr_t)leader % NOS_TASKS;
    ok &= !credential_receive((uintptr_t)endpoint, &message) && message.op == 6 && message.value == 1 &&
        !credential_subject(&message, &second) && second.process == (uint32_t)replacement && credential_expected(&second.credentials, 1);
    message.value = 1;
    ok &= !nos_syscall(NOS_SYS_REPLY, (uintptr_t)message.sender, (uintptr_t)&message, 0) && wait_for(replacement, 101);
    ok &= !credential_subject(&retained, &second) && credential_bytes(&first, &second, sizeof(first));
    retained.value = 1;
    ok &= !nos_syscall(NOS_SYS_REPLY, (uintptr_t)retained.sender, (uintptr_t)&retained, 0);
    ok &= !credential_receive((uintptr_t)endpoint, &message) && message.op == 5 && message.value == 1 &&
        !credential_subject(&message, &second) && credential_expected(&second.credentials, 0);
    message.value = 1;
    ok &= !nos_syscall(NOS_SYS_REPLY, (uintptr_t)message.sender, (uintptr_t)&message, 0);
    nos_check(ok, "credentials_leader_slot_lifetime");
    /* A thread has no waitable zombie. Its group-owned mappings disappear on exit. */
    for (unsigned attempt = 0; attempt < 1000; attempt++) {
        nos_syscall(NOS_SYS_OBJECT_STATS, (uintptr_t)&after, 0, 0);
        if (after.tasks == before.tasks) break;
        nos_syscall(NOS_SYS_SLEEP, 1, 0, 0);
    }
    ok &= !nos_syscall(NOS_SYS_ENDPOINT_CLOSE, (uintptr_t)endpoint, 0, 0);
    nos_syscall(NOS_SYS_OBJECT_STATS, (uintptr_t)&after, 0, 0);
    ok &= before.tasks == after.tasks && before.endpoints == after.endpoints && before.free_pages == after.free_pages &&
        !credential_get(&current) && credential_bytes(&initial, &current, sizeof(initial));
    nos_check(ok, "credentials_native_cleanup");
}
#else
static void nos_credentials_regression(void) { }
#endif
#endif
