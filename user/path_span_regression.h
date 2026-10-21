/* Copyright (C) 2026 NOS contributors. Private to kernel_regression.c. */
#ifndef NOS_PATH_SPAN_REGRESSION_H
#define NOS_PATH_SPAN_REGRESSION_H
#if defined(NOS_MMU) && defined(NOS_LINUX_COMPAT)
static int vector_bytes(const void *pointer, size_t count, unsigned char value)
{
    const unsigned char *p = pointer;
    for (size_t i = 0; i < count; i++) if (p[i] != value) return 0;
    return 1;
}
static unsigned char vector_pattern(unsigned i, unsigned seed)
{ return (unsigned char)((i * 13u) ^ seed); }
static int vector_pattern_is(const unsigned char *p, unsigned seed)
{
    for (unsigned i = 0; i < NOS_PAGE_SIZE; i++) if (p[i] != vector_pattern(i, seed)) return 0;
    return 1;
}
static int vector_receive(uintptr_t endpoint, struct nos_message *message)
{
    for (unsigned i = 0; i < 1000; i++) {
        intptr_t rc = nos_syscall(NOS_SYS_RECV, endpoint, (uintptr_t)message, 0);
        if (rc == -NOS_EACCES) { nos_syscall(NOS_SYS_SLEEP, 1, 0, 0); continue; }
        if (rc) return (int)rc;
        if (message->op >= 0) return 0;
    }
    return -NOS_ETIMEDOUT;
}
static intptr_t vector_call(struct nos_callv_args *args)
{ return nos_syscall(NOS_SYS_IPC_VECTOR, NOS_IPC_VECTOR_CALL, (uintptr_t)args, 0); }
static struct nos_callv_args vector_args(uintptr_t endpoint, struct nos_message *m)
{
    struct nos_callv_args args;
    u_memset(&args, 0, sizeof(args));
    args.endpoint = endpoint; args.request = args.reply = (uintptr_t)m; args.timeout_ticks = 1000;
    return args;
}
static int vector_transfer(struct nos_ipc_xfer *transfer, intptr_t expected)
{ return nos_syscall(NOS_SYS_IPC_XFER, (uintptr_t)transfer, 0, 0) == expected; }
static int vector_denied(const struct nos_message *message, unsigned direction, intptr_t expected)
{
    unsigned char output[8]; u_memset(output, 0xb6, sizeof(output));
    struct nos_ipc_xfer x = {message->sender, message->token, 0, (uintptr_t)output, sizeof(output), direction};
    return vector_transfer(&x, expected) && vector_bytes(output, sizeof(output), 0xb6);
}
static void vector_service(uintptr_t endpoint)
{
    intptr_t mapped = nos_syscall(NOS_SYS_MMAP, 3, NOS_PAGE_SIZE * 3, 0);
    if (nos_pointer_is_error(mapped)) nos_syscall(NOS_SYS_EXIT, 122, 0, 0);
    uintptr_t area = (uintptr_t)mapped;
    unsigned char *local = (void *)(area + 23);
    int all = !nos_syscall(NOS_SYS_MPROTECT, area + 2 * NOS_PAGE_SIZE, 0, NOS_PAGE_SIZE);
    for (;;) {
        struct nos_message m;
        if (vector_receive(endpoint, &m)) nos_syscall(NOS_SYS_EXIT, 122, 0, 0);
        int ok = 1;
        struct nos_ipc_xfer x = {m.sender, m.token, 0, (uintptr_t)local, NOS_PAGE_SIZE, NOS_IPC_FETCH};
        if (m.op == 1) {
            ok &= m.bulk_in_len == NOS_PAGE_SIZE && m.bulk_in != 1;
            u_memset((void *)area, 0xa9, NOS_PAGE_SIZE * 2);
            ok &= vector_transfer(&x, 0) && vector_pattern_is(local, 0x53);
            ok &= vector_bytes((void *)area, 23, 0xa9) &&
                vector_bytes(local + NOS_PAGE_SIZE, NOS_PAGE_SIZE - 23, 0xa9);
            x.direction = NOS_IPC_FETCH_SECOND;
            /* Changes to the server's received message cannot redirect spans. */
            m.bulk_in = 1; m.bulk_in_len = 1;
            ok &= vector_transfer(&x, 0) && vector_pattern_is(local, 0x97);
            x.offset = NOS_PAGE_SIZE - 1; x.count = 2;
            ok &= vector_transfer(&x, -NOS_EINVAL);
            x.offset = 0; x.count = NOS_PAGE_SIZE + 1;
            ok &= vector_transfer(&x, -NOS_EINVAL);
            x.count = 8; x.direction = 3;
            ok &= vector_transfer(&x, -NOS_EINVAL);
            x.direction = NOS_IPC_FETCH_SECOND; x.token++;
            ok &= vector_transfer(&x, -NOS_EPERM); x.token--;
            x.sender += NOS_TASKS;
            ok &= vector_transfer(&x, -NOS_EPERM); x.sender = m.sender;
            x.count = 32; x.buffer = area + 2 * NOS_PAGE_SIZE - 16;
            u_memset((void *)(area + 2 * NOS_PAGE_SIZE - 16), 0x4b, 16);
            ok &= vector_transfer(&x, -NOS_EFAULT) &&
                vector_bytes((void *)(area + 2 * NOS_PAGE_SIZE - 16), 16, 0x4b);
            x.buffer = (uintptr_t)local; x.count = NOS_PAGE_SIZE;
            u_memset((void *)area, 0xa9, NOS_PAGE_SIZE * 2);
            ok &= !nos_syscall(NOS_SYS_MPROTECT, area + NOS_PAGE_SIZE, NOS_PROT_READ, NOS_PAGE_SIZE);
            ok &= vector_transfer(&x, -NOS_EFAULT) && vector_bytes((void *)area, NOS_PAGE_SIZE * 2, 0xa9);
            ok &= !nos_syscall(NOS_SYS_MPROTECT, area + NOS_PAGE_SIZE, 3, NOS_PAGE_SIZE);
            for (unsigned i = 0; i < NOS_PAGE_SIZE; i++) local[i] = vector_pattern(i, 0xc7);
            m.bulk_out = 1; m.bulk_out_len = 1;
            x.direction = NOS_IPC_STORE;
            ok &= vector_transfer(&x, 0);
        } else if (m.op == 2) {
            /* STORE overwrites the caller's original descriptor structure.
             * Subsequent FETCH must keep using the kernel's captured inputs. */
            u_memset(local, 0xcc, sizeof(struct nos_callv_args));
            x.count = sizeof(struct nos_callv_args); x.direction = NOS_IPC_STORE;
            ok &= vector_transfer(&x, 0);
            x.count = 8; x.direction = NOS_IPC_FETCH_SECOND;
            ok &= vector_transfer(&x, 0);
            for (unsigned i = 0; i < 8; i++) ok &= local[i] == vector_pattern(i, 0x97);
            x.direction = NOS_IPC_FETCH;
            ok &= vector_transfer(&x, 0);
            for (unsigned i = 0; i < 8; i++) ok &= local[i] == vector_pattern(i, 0x53);
        } else if (m.op == 3) {
            x.buffer = UINTPTR_MAX; x.count = 0;
            ok &= vector_transfer(&x, m.arg ? 0 : -NOS_EINVAL);
            x.direction = NOS_IPC_FETCH_SECOND;
            ok &= vector_transfer(&x, m.arg == 2 ? 0 : -NOS_EINVAL);
            if (m.arg == 2) {
                x.offset = 1;
                ok &= vector_transfer(&x, -NOS_EINVAL);
            }
            x.offset = 0; x.direction = NOS_IPC_STORE;
            ok &= vector_transfer(&x, 0);
            local[0] = 0x6a; x.buffer = (uintptr_t)local; x.count = 1;
            ok &= vector_transfer(&x, 0);
        } else if (m.op == 4) {
            x.count = 8; x.direction = NOS_IPC_FETCH;
            ok &= vector_transfer(&x, 0);
            x.direction = NOS_IPC_FETCH_SECOND; x.count = 0;
            ok &= vector_transfer(&x, -NOS_EINVAL);
        } else if (m.op != 5) ok = 0;
        all &= ok; m.value = ok;
        int replied = !nos_syscall(NOS_SYS_REPLY, (uintptr_t)m.sender, (uintptr_t)&m, 0);
        all &= replied;
        x.direction = NOS_IPC_FETCH_SECOND; x.count = 8; x.buffer = (uintptr_t)local;
        all &= vector_transfer(&x, -NOS_EPERM);
        if (m.op == 5) {
            nos_syscall(NOS_SYS_MUNMAP, area, NOS_PAGE_SIZE * 3, 0);
            nos_syscall(NOS_SYS_EXIT, all ? 121 : 122, 0, 0);
        }
    }
}
static int vector_preflight(uintptr_t endpoint, uintptr_t first, uintptr_t second, uintptr_t guard)
{
    struct nos_message m = { .op = 1, .value = 0x7341 };
    struct nos_callv_args valid = vector_args(endpoint, &m), args;
    valid.input_count = 2;
    valid.inputs[0] = (struct nos_ipc_span){first, NOS_PAGE_SIZE};
    valid.inputs[1] = (struct nos_ipc_span){second, NOS_PAGE_SIZE};
    int ok = nos_syscall(NOS_SYS_IPC_VECTOR, NOS_IPC_VECTOR_QUERY, 0, 0) == 2 &&
        nos_syscall(NOS_SYS_IPC_VECTOR, NOS_IPC_VECTOR_QUERY, 1, 0) == -NOS_EINVAL &&
        nos_syscall(NOS_SYS_IPC_VECTOR, NOS_IPC_VECTOR_QUERY, 0, 1) == -NOS_EINVAL &&
        nos_syscall(NOS_SYS_IPC_VECTOR, 2, 0, 0) == -NOS_EINVAL &&
        nos_syscall(NOS_SYS_IPC_VECTOR, NOS_IPC_VECTOR_CALL, 1, 0) == -NOS_EFAULT &&
        nos_syscall(NOS_SYS_IPC_VECTOR, NOS_IPC_VECTOR_CALL, (uintptr_t)&valid, 1) == -NOS_EINVAL;
    ok &= nos_syscall(NOS_SYS_IPC_VECTOR, NOS_IPC_VECTOR_CALL, guard - 16, 0) == -NOS_EFAULT;
    args = valid; args.reserved = 1; ok &= vector_call(&args) == -NOS_EINVAL;
    args = valid; args.input_count = 3; ok &= vector_call(&args) == -NOS_EINVAL;
    args = valid; args.input_count = 1; ok &= vector_call(&args) == -NOS_EINVAL;
    args = valid; args.timeout_ticks = 100001; ok &= vector_call(&args) == -NOS_EINVAL;
    args = valid; args.inputs[0].length++; ok &= vector_call(&args) == -NOS_EINVAL;
    args = valid; args.inputs[1].length++; ok &= vector_call(&args) == -NOS_EINVAL;
    args = valid; args.inputs[0].address = UINT64_MAX - 15; ok &= vector_call(&args) == -NOS_EFAULT;
    args = valid; args.inputs[1].address = UINT64_MAX - 15; ok &= vector_call(&args) == -NOS_EFAULT;
    args = valid; args.inputs[1].address = guard - 16; args.inputs[1].length = 32;
    ok &= vector_call(&args) == -NOS_EFAULT;
    args = valid; args.inputs[0].address = guard - 16; args.inputs[0].length = 32;
    ok &= vector_call(&args) == -NOS_EFAULT;
    args = valid; args.request = 1; ok &= vector_call(&args) == -NOS_EFAULT;
    args = valid; args.reply = 1; ok &= vector_call(&args) == -NOS_EFAULT;
    args = valid; args.reply = guard - 16;
    ok &= vector_call(&args) == -NOS_EFAULT && vector_bytes((void *)(guard - 16), 16, 0xee);
    m.bulk_out = guard - 16; m.bulk_out_len = 32;
    args = valid; ok &= vector_call(&args) == -NOS_EFAULT;
    m.bulk_out = 0; m.bulk_out_len = NOS_IPC_BULK_MAX + 1;
    args = valid; ok &= vector_call(&args) == -NOS_EINVAL;
    m.bulk_out = m.bulk_out_len = 0;
#ifdef NOS_ARM
    args = valid; args.endpoint |= 1ull << 32; ok &= vector_call(&args) == -NOS_EINVAL;
    args = valid; args.request |= 1ull << 32; ok &= vector_call(&args) == -NOS_EFAULT;
    args = valid; args.reply |= 1ull << 32; ok &= vector_call(&args) == -NOS_EFAULT;
    args = valid; args.inputs[1].address |= 1ull << 32; ok &= vector_call(&args) == -NOS_EFAULT;
    args = vector_args(endpoint, &m); args.input_count = 1;
    args.inputs[0].address = 1ull << 32; ok &= vector_call(&args) == -NOS_EFAULT;
#endif
    return ok && m.value == 0x7341;
}
static void vector_lifetime_client(uintptr_t endpoint, unsigned mode)
{
    unsigned char first[8], second[8];
    for (unsigned i = 0; i < 8; i++) { first[i] = (unsigned char)i; second[i] = (unsigned char)(0x70 + i); }
    struct nos_message m = { .op = (intptr_t)mode };
    struct nos_callv_args args = vector_args(endpoint, &m);
    args.input_count = 2;
    args.inputs[0] = (struct nos_ipc_span){(uintptr_t)first, sizeof(first)};
    args.inputs[1] = (struct nos_ipc_span){(uintptr_t)second, sizeof(second)};
    if (mode == 4) args.timeout_ticks = 20;
    intptr_t rc = vector_call(&args);
    intptr_t expected = mode == 0 ? 0 : mode == 1 ? -NOS_EINTR :
        mode == 3 ? -NOS_ESRCH : mode == 4 ? -NOS_ETIMEDOUT : -NOS_EACCES;
    nos_syscall(NOS_SYS_EXIT, rc == expected ? 123 : 124, 0, 0);
}
static void vector_reply_client(uintptr_t endpoint) { vector_lifetime_client(endpoint, 0); }
static void vector_cancel_client(uintptr_t endpoint) { vector_lifetime_client(endpoint, 1); }
static void vector_revoke_client(uintptr_t endpoint) { vector_lifetime_client(endpoint, 2); }
static void vector_close_client(uintptr_t endpoint) { vector_lifetime_client(endpoint, 3); }
static void vector_timeout_client(uintptr_t endpoint) { vector_lifetime_client(endpoint, 4); }
static void vector_wrong_server(uintptr_t endpoint)
{
    struct nos_message m;
    if (vector_receive(endpoint, &m)) nos_syscall(NOS_SYS_EXIT, 126, 0, 0);
    struct nos_message original = { .sender = m.value };
    u_memcpy(&original.token, m.data, sizeof(original.token));
    m.value = vector_denied(&original, NOS_IPC_FETCH_SECOND, -NOS_EPERM);
    int ok = !nos_syscall(NOS_SYS_REPLY, (uintptr_t)m.sender, (uintptr_t)&m, 0);
    nos_syscall(NOS_SYS_EXIT, ok ? 125 : 126, 0, 0);
}
static int vector_lifetimes(void)
{
    intptr_t endpoint = nos_syscall(NOS_SYS_ENDPOINT_CREATE, 0, 0, 0);
    if (endpoint < 0) return 0;
    void (*const clients[])(uintptr_t) = {vector_reply_client, vector_cancel_client,
        vector_revoke_client, vector_timeout_client, vector_close_client};
    struct nos_message expired = {0};
    int ok = 1;
    for (unsigned mode = 0; mode < 5; mode++) {
        intptr_t client = spawn(clients[mode], (uintptr_t)endpoint, 6);
        struct nos_message m;
        if (client < 0 || vector_receive((uintptr_t)endpoint, &m)) {
            nos_syscall(NOS_SYS_ENDPOINT_CLOSE, (uintptr_t)endpoint, 0, 0);
            if (client >= 0) {
                nos_syscall(NOS_SYS_TASK_CANCEL, (uintptr_t)client, 0, 0);
                intptr_t status; nos_syscall(NOS_SYS_WAIT, (uintptr_t)client, (uintptr_t)&status, 0);
            }
            return 0;
        }
        struct nos_ipc_xfer x;
        unsigned char output[8];
        x = (struct nos_ipc_xfer){m.sender, m.token, 0, (uintptr_t)output, sizeof(output), NOS_IPC_FETCH_SECOND};
        ok &= vector_transfer(&x, 0);
        for (unsigned i = 0; i < 8; i++) ok &= output[i] == (unsigned char)(0x70 + i);
        if (!mode) {
            struct nos_message wrong = m; wrong.token++;
            ok &= vector_denied(&wrong, NOS_IPC_FETCH_SECOND, -NOS_EPERM);
            wrong = m; wrong.sender += NOS_TASKS;
            ok &= vector_denied(&wrong, NOS_IPC_FETCH_SECOND, -NOS_EPERM);
            intptr_t other_endpoint = nos_syscall(NOS_SYS_ENDPOINT_CREATE, 0, 0, 0);
            intptr_t other = other_endpoint < 0 ? other_endpoint : spawn(vector_wrong_server, (uintptr_t)other_endpoint, 6);
            int authority = other >= 0 && !nos_syscall(NOS_SYS_ENDPOINT_GRANT, (uintptr_t)other,
                (uintptr_t)other_endpoint, NOS_RIGHT_RECV);
            struct nos_message probe = {.op = 1, .value = m.sender, .len = sizeof(m.token)};
            u_memcpy(probe.data, &m.token, sizeof(m.token));
            if (other >= 0) {
                struct nos_call_args call = {(uintptr_t)other_endpoint, (uintptr_t)&probe, (uintptr_t)&probe, 1000};
                intptr_t rc = nos_syscall(NOS_SYS_CALL_EX, (uintptr_t)&call, 0, 0);
                if (rc) nos_syscall(NOS_SYS_TASK_CANCEL, (uintptr_t)other, 0, 0);
                int reaped = wait_for(other, 125);
                authority &= !rc && probe.value == 1 && reaped;
            }
            if (other_endpoint >= 0) nos_syscall(NOS_SYS_ENDPOINT_CLOSE, (uintptr_t)other_endpoint, 0, 0);
            ok &= authority;
            ok &= !nos_syscall(NOS_SYS_REPLY, (uintptr_t)m.sender, (uintptr_t)&m, 0);
        } else {
            ok &= vector_denied(&expired, NOS_IPC_FETCH_SECOND, -NOS_EPERM);
            if (mode == 1) ok &= !nos_syscall(NOS_SYS_IPC_CANCEL, (uintptr_t)client, 0, 0);
            else if (mode == 2) ok &= !nos_syscall(NOS_SYS_ENDPOINT_REVOKE,
                (uintptr_t)nos_syscall(NOS_SYS_GETPID, 0, 0, 0), (uintptr_t)endpoint, NOS_RIGHT_RECV);
            else if (mode == 3) nos_syscall(NOS_SYS_SLEEP, 22, 0, 0);
            else ok &= !nos_syscall(NOS_SYS_ENDPOINT_CLOSE, (uintptr_t)endpoint, 0, 0);
        }
        ok &= vector_denied(&m, NOS_IPC_FETCH_SECOND, -NOS_EPERM);
        int reaped = wait_for(client, 123); ok &= reaped;
        if (mode == 2) ok &= !nos_syscall(NOS_SYS_ENDPOINT_GRANT,
            (uintptr_t)nos_syscall(NOS_SYS_GETPID, 0, 0, 0), (uintptr_t)endpoint, NOS_RIGHT_RECV);
        expired = m;
    }
    return ok;
}
static void nos_path_span_regression(void)
{
    struct nos_object_stats before, after;
    nos_syscall(NOS_SYS_OBJECT_STATS, (uintptr_t)&before, 0, 0);
    intptr_t endpoint = nos_syscall(NOS_SYS_ENDPOINT_CREATE, 0, 0, 0);
    intptr_t server = endpoint < 0 ? endpoint : spawn(vector_service, (uintptr_t)endpoint, 6);
    uintptr_t areas[3] = {0};
    int ok = server >= 0 && !nos_syscall(NOS_SYS_ENDPOINT_GRANT, (uintptr_t)server, (uintptr_t)endpoint, NOS_RIGHT_RECV);
    for (unsigned j = 0; j < 3; j++) {
        intptr_t area = nos_syscall(NOS_SYS_MMAP, 3, NOS_PAGE_SIZE * 3, 0);
        if (nos_pointer_is_error(area)) { ok = 0; break; }
        areas[j] = (uintptr_t)area;
        u_memset((void *)areas[j], 0xee, NOS_PAGE_SIZE * 2);
        ok &= !nos_syscall(NOS_SYS_MPROTECT, areas[j] + 2 * NOS_PAGE_SIZE, 0, NOS_PAGE_SIZE);
    }
    if (!ok) {
        nos_check(0, "ipc_vector_query_and_preflight");
        if (server >= 0) { nos_syscall(NOS_SYS_TASK_CANCEL, (uintptr_t)server, 0, 0); wait_for(server, -NOS_EINTR); }
        for (unsigned j = 0; j < 3; j++) if (areas[j]) nos_syscall(NOS_SYS_MUNMAP, areas[j], NOS_PAGE_SIZE * 3, 0);
        if (endpoint >= 0) nos_syscall(NOS_SYS_ENDPOINT_CLOSE, (uintptr_t)endpoint, 0, 0);
        return;
    }
    unsigned char *first = (void *)(areas[0] + 17), *second = (void *)(areas[1] + 31), *output = (void *)(areas[2] + 13);
    for (unsigned i = 0; i < NOS_PAGE_SIZE; i++) {
        first[i] = vector_pattern(i, 0x53); second[i] = vector_pattern(i, 0x97);
    }
    nos_check(vector_preflight((uintptr_t)endpoint, (uintptr_t)first, (uintptr_t)second,
        areas[0] + 2 * NOS_PAGE_SIZE), "ipc_vector_query_and_preflight");
    ok = !nos_syscall(NOS_SYS_MPROTECT, areas[0], NOS_PROT_READ, NOS_PAGE_SIZE * 2) &&
        !nos_syscall(NOS_SYS_MPROTECT, areas[1], NOS_PROT_READ, NOS_PAGE_SIZE * 2);
    struct nos_message m = {.op = 1, .bulk_in = 1, .bulk_in_len = 1,
        .bulk_out = (uintptr_t)output, .bulk_out_len = NOS_PAGE_SIZE};
    struct nos_callv_args args = vector_args((uintptr_t)endpoint, &m);
    args.input_count = 2;
    args.inputs[0] = (struct nos_ipc_span){(uintptr_t)first, NOS_PAGE_SIZE};
    args.inputs[1] = (struct nos_ipc_span){(uintptr_t)second, NOS_PAGE_SIZE};
    ok &= !vector_call(&args) && m.value == 1 && vector_pattern_is(output, 0xc7) &&
        vector_pattern_is(first, 0x53) && vector_pattern_is(second, 0x97) &&
        vector_bytes((void *)areas[2], 13, 0xee) &&
        vector_bytes(output + NOS_PAGE_SIZE, NOS_PAGE_SIZE - 13, 0xee);
    m = (struct nos_message){.op = 2, .bulk_out = (uintptr_t)&args, .bulk_out_len = sizeof(args)};
    args = vector_args((uintptr_t)endpoint, &m); args.input_count = 2;
    args.inputs[0] = (struct nos_ipc_span){(uintptr_t)first, 8};
    args.inputs[1] = (struct nos_ipc_span){(uintptr_t)second, 8};
    ok &= !vector_call(&args) && m.value == 1 && vector_bytes(&args, sizeof(args), 0xcc);
    nos_check(ok, "ipc_vector_two_spans_guard_permissions");
    ok = 1;
    for (unsigned count = 0; count <= 2; count++) {
        unsigned char zero_output = 0;
        m = (struct nos_message){.op = 3, .arg = count, .bulk_out = (uintptr_t)&zero_output, .bulk_out_len = 1};
        args = vector_args((uintptr_t)endpoint, &m); args.input_count = count;
        unsigned char unaligned[sizeof(args) + 1];
        u_memcpy(unaligned + 1, &args, sizeof(args));
        intptr_t result = nos_syscall(NOS_SYS_IPC_VECTOR, NOS_IPC_VECTOR_CALL, (uintptr_t)(unaligned + 1), 0);
        ok &= !result && m.value == 1 && zero_output == 0x6a;
    }
    m = (struct nos_message){.op = 4, .bulk_in = (uintptr_t)first, .bulk_in_len = 8};
    struct nos_call_args legacy = {(uintptr_t)endpoint, (uintptr_t)&m, (uintptr_t)&m, 1000};
    ok &= !nos_syscall(NOS_SYS_CALL_EX, (uintptr_t)&legacy, 0, 0) && m.value == 1;
    nos_check(ok, "ipc_vector_zero_and_legacy_separation");
    m = (struct nos_message){.op = 5}; args = vector_args((uintptr_t)endpoint, &m);
    intptr_t rc = vector_call(&args);
    if (rc) nos_syscall(NOS_SYS_TASK_CANCEL, (uintptr_t)server, 0, 0);
    int reaped = wait_for(server, 121);
    ok = !rc && m.value == 1 && reaped;
    /* RECV ownership moved to the service; its exit already reclaimed the
     * endpoint. The old generation must be invalid rather than close twice. */
    if (endpoint >= 0) ok &= nos_syscall(NOS_SYS_ENDPOINT_CLOSE, (uintptr_t)endpoint, 0, 0) == -NOS_EINVAL;
    for (unsigned j = 0; j < 3; j++) ok &= !nos_syscall(NOS_SYS_MUNMAP, areas[j], NOS_PAGE_SIZE * 3, 0);
    nos_check(vector_lifetimes(), "ipc_vector_authority_lifetime");
    nos_syscall(NOS_SYS_OBJECT_STATS, (uintptr_t)&after, 0, 0);
    nos_check(ok && before.tasks == after.tasks && before.endpoints == after.endpoints &&
        before.free_pages == after.free_pages, "ipc_vector_cleanup");
}
#else
static void nos_path_span_regression(void) { }
#endif
#endif
