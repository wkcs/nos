/* Copyright (C) 2026 NOS contributors */
#include "abi.h"
static intptr_t spawn(void (*entry)(uintptr_t), uintptr_t arg, unsigned priority)
{ return nos_syscall(NOS_SYS_SPAWN, (uintptr_t)entry, arg, priority); }
static int wait_for(intptr_t pid, intptr_t expected)
{
    intptr_t status = 0;
    return pid >= 0 && !nos_syscall(NOS_SYS_WAIT, (uintptr_t)pid, (uintptr_t)&status, 0) && status == expected;
}
static void endpoint_service(uintptr_t endpoint)
{
    nos_syscall(NOS_SYS_SLEEP, 1, 0, 0);
    for (;;) {
        struct nos_message message;
        intptr_t rc = nos_syscall(NOS_SYS_RECV, endpoint, (uintptr_t)&message, 0);
        if (rc == -NOS_EACCES) { nos_syscall(NOS_SYS_SLEEP, 1, 0, 0); continue; }
        if (rc) nos_syscall(NOS_SYS_EXIT, 70, 0, 0);
        if (message.op == NOS_IPC_PEER_CANCEL || message.op == NOS_IPC_PEER_EXIT) continue;
        intptr_t operation = message.op;
        if (operation == 2 || operation == 3) nos_syscall(NOS_SYS_SLEEP, 5, 0, 0);
        if (operation == 4) message.value = nos_syscall(NOS_SYS_MUTEX_LOCK, (uintptr_t)message.arg, 0, 0);
        else message.value += 10;
        rc = nos_syscall(NOS_SYS_REPLY, (uintptr_t)message.sender, (uintptr_t)&message, 0);
        /* A timed-out reply may be rejected; it must never match a newer call. */
        if (operation == 3 && rc != -NOS_EFAULT) nos_syscall(NOS_SYS_EXIT, 72, 0, 0);
        if (rc && operation != 2 && operation != 3) nos_syscall(NOS_SYS_EXIT, 71, 0, 0);
        if (operation == 99) nos_syscall(NOS_SYS_EXIT, 57, 0, 0);
    }
}
static void lock_chain_worker(uintptr_t lock)
{
    nos_syscall(NOS_SYS_MUTEX_LOCK, lock, 0, 0);
    nos_syscall(NOS_SYS_MUTEX_UNLOCK, lock, 0, 0);
    nos_syscall(NOS_SYS_EXIT, 33, 0, 0);
}
static void lock_waiter(uintptr_t lock)
{
    nos_syscall(NOS_SYS_MUTEX_LOCK, lock, 0, 0);
    nos_syscall(NOS_SYS_MUTEX_UNLOCK, lock, 0, 0);
    nos_syscall(NOS_SYS_EXIT, 34, 0, 0);
}
static void transitive_worker(uintptr_t endpoint)
{
    struct nos_message request;
    intptr_t rc;
    do {
        rc = nos_syscall(NOS_SYS_RECV, endpoint, (uintptr_t)&request, 0);
        if (rc == -NOS_EACCES) nos_syscall(NOS_SYS_SLEEP, 1, 0, 0);
    } while (rc == -NOS_EACCES);
    if (rc || nos_syscall(NOS_SYS_MUTEX_LOCK, (uintptr_t)request.arg, 0, 0))
        nos_syscall(NOS_SYS_EXIT, 46, 0, 0);
    nos_syscall(NOS_SYS_REPLY, (uintptr_t)request.sender, (uintptr_t)&request, 0);
    rc = nos_syscall(NOS_SYS_MUTEX_LOCK, (uintptr_t)request.value, 0, 0);
    int ok = !rc && nos_syscall(NOS_SYS_PRIORITY, 0, 0, 0) == 4;
    nos_syscall(NOS_SYS_MUTEX_UNLOCK, (uintptr_t)request.value, 0, 0);
    /* Donation from the high-priority waiter survives releasing another lock. */
    ok &= nos_syscall(NOS_SYS_PRIORITY, 0, 0, 0) == 4;
    nos_syscall(NOS_SYS_MUTEX_UNLOCK, (uintptr_t)request.arg, 0, 0);
    nos_syscall(NOS_SYS_EXIT, ok ? 45 : 46, 0, 0);
}
static void orphan_child(uintptr_t unused)
{
    (void)unused;
    nos_syscall(NOS_SYS_SLEEP, 3, 0, 0);
    nos_syscall(NOS_SYS_EXIT, 61, 0, 0);
}
static void orphan_parent(uintptr_t unused)
{
    (void)unused;
    while (!(nos_syscall(NOS_SYS_CAPS, 0, 0, 0) & NOS_CAP_SPAWN)) nos_syscall(NOS_SYS_SLEEP, 1, 0, 0);
    intptr_t child = spawn(orphan_child, 0, 6);
    nos_syscall(NOS_SYS_EXIT, child < 0 ? 62 : 60, 0, 0);
}
struct lazy_mutex_probe { unsigned gate, tid; intptr_t lock, result; };
static void lazy_mutex_thread(uintptr_t pointer)
{
    struct lazy_mutex_probe *probe = (void *)pointer;
    while (!__atomic_load_n(&probe->gate, __ATOMIC_ACQUIRE))
        nos_syscall(NOS_SYS_FUTEX_WAIT, (uintptr_t)&probe->gate, 0, 0);
    intptr_t rc = nos_syscall(NOS_SYS_MUTEX_LOCK, (uintptr_t)probe->lock, 0, 0);
    if (!rc) rc = nos_syscall(NOS_SYS_MUTEX_UNLOCK, (uintptr_t)probe->lock, 0, 0);
    probe->result = rc;
    nos_syscall(NOS_SYS_EXIT, 0, 0, 0);
}
struct reply_unmap_probe { uintptr_t address; unsigned tid; };
static void reply_unmap_thread(uintptr_t pointer)
{
    struct reply_unmap_probe *probe = (void *)pointer;
    nos_syscall(NOS_SYS_SLEEP, 1, 0, 0);
    nos_syscall(NOS_SYS_MUNMAP, probe->address, NOS_PAGE_SIZE, 0);
    nos_syscall(NOS_SYS_EXIT, 0, 0, 0);
}
#ifdef NOS_LINUX_COMPAT
struct bulk_control { uintptr_t endpoint, target, address; unsigned mode, tid; };
static void bulk_control_thread(uintptr_t pointer)
{
    struct bulk_control *control = (void *)pointer; struct nos_message message;
    intptr_t rc;
    do {
        rc = nos_syscall(NOS_SYS_RECV, control->endpoint, (uintptr_t)&message, 0);
        if (rc == -NOS_EACCES) nos_syscall(NOS_SYS_SLEEP, 1, 0, 0);
    } while (rc == -NOS_EACCES);
    if (!rc) {
        message.value = control->mode == 1 ? nos_syscall(NOS_SYS_MUNMAP, control->address, NOS_PAGE_SIZE, 0) :
            nos_syscall(NOS_SYS_ENDPOINT_REVOKE, control->target, control->address, NOS_RIGHT_RECV);
        nos_syscall(NOS_SYS_REPLY, (uintptr_t)message.sender, (uintptr_t)&message, 0);
    }
    nos_syscall(NOS_SYS_EXIT, 0, 0, 0);
}
static void bulk_service(uintptr_t endpoint)
{
    int ok = 1;
    for (;;) {
        struct nos_message message; intptr_t rc;
        do {
            rc = nos_syscall(NOS_SYS_RECV, endpoint, (uintptr_t)&message, 0);
            if (rc == -NOS_EACCES) nos_syscall(NOS_SYS_SLEEP, 1, 0, 0);
        } while (rc == -NOS_EACCES);
        if (rc) nos_syscall(NOS_SYS_EXIT, 76, 0, 0);
        if (message.op < 0) continue;
        unsigned char buffer[32];
        struct nos_ipc_xfer transfer = {message.sender, message.token, 4, (uintptr_t)buffer, 8, NOS_IPC_FETCH};
        if (message.op == 1) {
            ok &= !nos_syscall(NOS_SYS_IPC_XFER, (uintptr_t)&transfer, 0, 0);
            for (unsigned i = 0; i < 8; i++) ok &= buffer[i] == ((i + 4) ^ 0x5a);
            transfer.offset = 95; transfer.count = 2;
            ok &= nos_syscall(NOS_SYS_IPC_XFER, (uintptr_t)&transfer, 0, 0) == -NOS_EINVAL;
            transfer.offset = 0; transfer.count = NOS_IPC_BULK_MAX + 1;
            ok &= nos_syscall(NOS_SYS_IPC_XFER, (uintptr_t)&transfer, 0, 0) == -NOS_EINVAL;
            transfer.count = 8; transfer.direction = 2;
            ok &= nos_syscall(NOS_SYS_IPC_XFER, (uintptr_t)&transfer, 0, 0) == -NOS_EINVAL;
            transfer.direction = NOS_IPC_FETCH; transfer.token++;
            ok &= nos_syscall(NOS_SYS_IPC_XFER, (uintptr_t)&transfer, 0, 0) == -NOS_EPERM;
            transfer.token--; transfer.sender += NOS_TASKS;
            ok &= nos_syscall(NOS_SYS_IPC_XFER, (uintptr_t)&transfer, 0, 0) == -NOS_EPERM;
            transfer.sender = message.sender; transfer.buffer = 1;
            ok &= nos_syscall(NOS_SYS_IPC_XFER, (uintptr_t)&transfer, 0, 0) == -NOS_EFAULT;
            intptr_t scratch = nos_syscall(NOS_SYS_MMAP, NOS_PROT_READ | NOS_PROT_WRITE, NOS_PAGE_SIZE, 0);
            if (nos_pointer_is_error(scratch)) ok = 0;
            else {
                transfer.buffer = (uintptr_t)scratch + NOS_PAGE_SIZE - 4;
                ok &= nos_syscall(NOS_SYS_IPC_XFER, (uintptr_t)&transfer, 0, 0) == -NOS_EFAULT;
                nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)scratch, NOS_PAGE_SIZE, 0);
            }
            message.bulk_in = 1; transfer.buffer = (uintptr_t)buffer;
            ok &= !nos_syscall(NOS_SYS_IPC_XFER, (uintptr_t)&transfer, 0, 0);
            for (unsigned i = 0; i < 32; i++) buffer[i] = (unsigned char)(0xa0 + i);
            transfer.direction = NOS_IPC_STORE; transfer.count = 32;
            ok &= !nos_syscall(NOS_SYS_IPC_XFER, (uintptr_t)&transfer, 0, 0);
            message.value = ok;
            nos_syscall(NOS_SYS_REPLY, (uintptr_t)message.sender, (uintptr_t)&message, 0);
            ok &= nos_syscall(NOS_SYS_IPC_XFER, (uintptr_t)&transfer, 0, 0) == -NOS_EPERM;
        } else if (message.op == 2) {
            nos_syscall(NOS_SYS_SLEEP, 5, 0, 0);
            ok &= nos_syscall(NOS_SYS_IPC_XFER, (uintptr_t)&transfer, 0, 0) == -NOS_EPERM;
        } else if (message.op == 3) {
            ok &= !nos_syscall(NOS_SYS_IPC_XFER, (uintptr_t)&transfer, 0, 0) && buffer[0] == 0x55;
            message.value = ok; nos_syscall(NOS_SYS_REPLY, (uintptr_t)message.sender, (uintptr_t)&message, 0);
        } else {
            struct nos_message control = {.op = 1};
            ok &= !nos_syscall(NOS_SYS_CALL, (uintptr_t)message.arg, (uintptr_t)&control, (uintptr_t)&control) && !control.value;
            int expected = message.op == 4 ? -NOS_EFAULT : -NOS_EPERM;
            ok &= nos_syscall(NOS_SYS_IPC_XFER, (uintptr_t)&transfer, 0, 0) == expected;
            transfer.direction = NOS_IPC_STORE;
            ok &= nos_syscall(NOS_SYS_IPC_XFER, (uintptr_t)&transfer, 0, 0) == expected;
            if (message.op == 5) nos_syscall(NOS_SYS_EXIT, ok ? 75 : 76, 0, 0);
            message.value = ok; nos_syscall(NOS_SYS_REPLY, (uintptr_t)message.sender, (uintptr_t)&message, 0);
        }
    }
}
static void bulk_regression(void)
{
    unsigned char input[96], output[34];
    for (unsigned i = 0; i < sizeof(input); i++) input[i] = (unsigned char)(i ^ 0x5a);
    u_memset(output, 0xee, sizeof(output));
    intptr_t endpoint = nos_syscall(NOS_SYS_ENDPOINT_CREATE, 0, 0, 0);
    intptr_t server = spawn(bulk_service, (uintptr_t)endpoint, 6);
    int ok = server >= 0 && endpoint >= 0 && !nos_syscall(NOS_SYS_ENDPOINT_GRANT, (uintptr_t)server, (uintptr_t)endpoint, NOS_RIGHT_RECV);
    struct nos_message message = {.op = 1, .bulk_in = 1, .bulk_in_len = 8};
    ok &= nos_syscall(NOS_SYS_CALL, (uintptr_t)endpoint, (uintptr_t)&message, (uintptr_t)&message) == -NOS_EFAULT;
    message.bulk_in = (uintptr_t)input; message.bulk_in_len = NOS_IPC_BULK_MAX + 1;
    ok &= nos_syscall(NOS_SYS_CALL, (uintptr_t)endpoint, (uintptr_t)&message, (uintptr_t)&message) == -NOS_EINVAL;
    message.bulk_in_len = sizeof(input); message.bulk_out = 1; message.bulk_out_len = 8;
    ok &= nos_syscall(NOS_SYS_CALL, (uintptr_t)endpoint, (uintptr_t)&message, (uintptr_t)&message) == -NOS_EFAULT;
    message.bulk_out = (uintptr_t)(output + 1); message.bulk_out_len = 32;
    ok &= !nos_syscall(NOS_SYS_CALL, (uintptr_t)endpoint, (uintptr_t)&message, (uintptr_t)&message) && message.value == 1;
    for (unsigned i = 0; i < 32; i++) ok &= output[i + 1] == (unsigned char)(0xa0 + i);
    ok &= output[0] == 0xee && output[33] == 0xee;
    nos_check(ok, "ipc_bulk_token_bounds");
    message = (struct nos_message){.op = 2, .bulk_in = (uintptr_t)input, .bulk_in_len = sizeof(input)};
    struct nos_call_args call = {(uintptr_t)endpoint, (uintptr_t)&message, (uintptr_t)&message, 2};
    ok &= nos_syscall(NOS_SYS_CALL_EX, (uintptr_t)&call, 0, 0) == -NOS_ETIMEDOUT;
    input[4] = 0x55;
    message = (struct nos_message){.op = 3, .bulk_in = (uintptr_t)input, .bulk_in_len = sizeof(input)};
    ok &= !nos_syscall(NOS_SYS_CALL, (uintptr_t)endpoint, (uintptr_t)&message, (uintptr_t)&message) && message.value == 1;
    for (unsigned mode = 1; mode <= 2; mode++) {
        intptr_t payload = nos_syscall(NOS_SYS_MMAP, 3, NOS_PAGE_SIZE, 0);
        intptr_t stack = nos_syscall(NOS_SYS_MMAP, 3, NOS_PAGE_SIZE, 0);
        intptr_t control_endpoint = nos_syscall(NOS_SYS_ENDPOINT_CREATE, 0, 0, 0);
        struct bulk_control control = {(uintptr_t)control_endpoint, (uintptr_t)server, mode == 1 ? (uintptr_t)payload : (uintptr_t)endpoint, mode, 0};
        struct nos_thread_args args = {(uintptr_t)bulk_control_thread, (uintptr_t)&control, (uintptr_t)stack, NOS_PAGE_SIZE, 0,
                                      (uintptr_t)&control.tid, (uintptr_t)&control.tid};
        intptr_t worker = nos_syscall(NOS_SYS_THREAD_CREATE, (uintptr_t)&args, UINTPTR_MAX, 0);
        ok &= worker >= 0 && !nos_syscall(NOS_SYS_TASK_AUTHORIZE, (uintptr_t)worker, NOS_CAP_SPAWN, 0) &&
              !nos_syscall(NOS_SYS_ENDPOINT_GRANT, (uintptr_t)worker, (uintptr_t)control_endpoint, NOS_RIGHT_RECV) &&
              !nos_syscall(NOS_SYS_ENDPOINT_GRANT, (uintptr_t)server, (uintptr_t)control_endpoint, NOS_RIGHT_CALL);
        message = (struct nos_message){.op = 3 + mode, .arg = control_endpoint, .bulk_in = (uintptr_t)payload, .bulk_in_len = 96,
                                      .bulk_out = (uintptr_t)payload + 128, .bulk_out_len = 96};
        intptr_t rc = nos_syscall(NOS_SYS_CALL, (uintptr_t)endpoint, (uintptr_t)&message, (uintptr_t)&message);
        ok &= mode == 1 ? !rc && message.value == 1 : rc == -NOS_EACCES;
        while (__atomic_load_n(&control.tid, __ATOMIC_ACQUIRE))
            nos_syscall(NOS_SYS_FUTEX_WAIT, (uintptr_t)&control.tid, control.tid, 0);
        nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)stack, NOS_PAGE_SIZE, 0);
        if (mode == 2) nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)payload, NOS_PAGE_SIZE, 0);
    }
    ok &= wait_for(server, 75);
    nos_syscall(NOS_SYS_ENDPOINT_CLOSE, (uintptr_t)endpoint, 0, 0);
    nos_check(ok, "ipc_bulk_cancel_revocation");
}
#endif
void nos_kernel_regression(void)
{
    struct nos_abi_info abi;
    nos_check(!nos_syscall(NOS_SYS_ABI_QUERY, (uintptr_t)&abi, 0, 0) &&
        abi.version == NOS_ABI_VERSION && abi.word_bits == sizeof(uintptr_t) * 8 &&
        abi.message_bytes == sizeof(struct nos_message), "versioned_syscall_abi");
    struct nos_object_stats before, after;
    nos_syscall(NOS_SYS_OBJECT_STATS, (uintptr_t)&before, 0, 0);
    intptr_t endpoint = nos_syscall(NOS_SYS_ENDPOINT_CREATE, 0, 0, 0);
    intptr_t child = spawn(endpoint_service, (uintptr_t)endpoint, 6);
    int ok = endpoint >= 0 && child >= 0 && !nos_syscall(NOS_SYS_ENDPOINT_GRANT,
        (uintptr_t)child, (uintptr_t)endpoint, NOS_RIGHT_RECV);
    struct nos_message message = { .op = 1, .value = 9 };
    ok &= !nos_syscall(NOS_SYS_CALL, (uintptr_t)endpoint, (uintptr_t)&message, (uintptr_t)&message) && message.value == 19;
    nos_check(ok, "multiple_endpoint_rpc");
    message.op = 2; message.value = 3;
    struct nos_call_args args = { (uintptr_t)endpoint, (uintptr_t)&message, (uintptr_t)&message, 2 };
    ok = nos_syscall(NOS_SYS_CALL_EX, (uintptr_t)&args, 0, 0) == -NOS_ETIMEDOUT;
    message.op = 1; message.value = 4;
    ok &= !nos_syscall(NOS_SYS_CALL, (uintptr_t)endpoint, (uintptr_t)&message, (uintptr_t)&message) && message.value == 14;
    nos_check(ok, "ipc_timeout_stale_reply");
    nos_check(!nos_syscall(NOS_SYS_ENDPOINT_REVOKE, 1, (uintptr_t)endpoint, NOS_RIGHT_CALL) &&
        nos_syscall(NOS_SYS_CALL, (uintptr_t)endpoint, (uintptr_t)&message, (uintptr_t)&message) == -NOS_EACCES,
        "endpoint_capability_revocation");
    nos_syscall(NOS_SYS_ENDPOINT_GRANT, 1, (uintptr_t)endpoint, NOS_RIGHT_CALL);
    message.op = 99;
    ok = !nos_syscall(NOS_SYS_CALL, (uintptr_t)endpoint, (uintptr_t)&message, (uintptr_t)&message) && wait_for(child, 57);
    ok &= nos_syscall(NOS_SYS_CALL, (uintptr_t)endpoint, (uintptr_t)&message, (uintptr_t)&message) == -NOS_EACCES;
    nos_syscall(NOS_SYS_OBJECT_STATS, (uintptr_t)&after, 0, 0);
    nos_check(ok && after.endpoints == before.endpoints, "endpoint_exit_reclamation");

#if UINTPTR_MAX > UINT32_MAX
    endpoint = nos_syscall(NOS_SYS_ENDPOINT_CREATE, 0, 0, 0);
    ok = endpoint >= 0 && nos_syscall(NOS_SYS_ENDPOINT_GRANT, 1, (uintptr_t)endpoint,
        ((uintptr_t)1 << 32) | NOS_RIGHT_CALL) == -NOS_EINVAL;
    ok &= nos_syscall(NOS_SYS_TASK_AUTHORIZE, 1, ((uintptr_t)1 << 32) | NOS_CAP_SPAWN, 0) == -NOS_EACCES;
    child = spawn(endpoint_service, (uintptr_t)endpoint, 6);
    ok &= child >= 0 && !nos_syscall(NOS_SYS_ENDPOINT_GRANT,
        (uintptr_t)child, (uintptr_t)endpoint, NOS_RIGHT_RECV);
    message = (struct nos_message){ .op = 99 };
    ok &= !nos_syscall(NOS_SYS_CALL, (uintptr_t)endpoint, (uintptr_t)&message, (uintptr_t)&message) && wait_for(child, 57);
    nos_check(ok, "capability_high_bits_validation");
#endif
    message = (struct nos_message){ .op = NOS_IPC_PEER_CANCEL };
    nos_check(nos_syscall(NOS_SYS_CALL, 0, (uintptr_t)&message, (uintptr_t)&message) == -NOS_EPERM,
              "ipc_reserved_cancel_validation");
    endpoint = nos_syscall(NOS_SYS_ENDPOINT_CREATE, 0, 0, 0);
    child = spawn(endpoint_service, (uintptr_t)endpoint, 6);
    ok = endpoint >= 0 && child >= 0 && !nos_syscall(NOS_SYS_ENDPOINT_GRANT,
        (uintptr_t)child, (uintptr_t)endpoint, NOS_RIGHT_RECV);
    message = (struct nos_message){ .op = 1 };
    ok &= !nos_syscall(NOS_SYS_CALL, (uintptr_t)endpoint, (uintptr_t)&message, (uintptr_t)&message);
    ok &= !nos_syscall(NOS_SYS_ENDPOINT_REVOKE, (uintptr_t)child, (uintptr_t)endpoint, NOS_RIGHT_RECV);
    ok &= nos_syscall(NOS_SYS_CALL, (uintptr_t)endpoint, (uintptr_t)&message, (uintptr_t)&message) == -NOS_ESRCH;
    ok &= !nos_syscall(NOS_SYS_TASK_CANCEL, (uintptr_t)child, 0, 0) && wait_for(child, -NOS_EINTR);
    nos_check(ok, "endpoint_receive_right_revocation");

    intptr_t cycle_lock = nos_syscall(NOS_SYS_MUTEX_CREATE, 0, 0, 0);
    endpoint = nos_syscall(NOS_SYS_ENDPOINT_CREATE, 0, 0, 0);
    child = spawn(endpoint_service, (uintptr_t)endpoint, 6);
    ok = cycle_lock >= 0 && endpoint >= 0 && child >= 0 &&
        !nos_syscall(NOS_SYS_ENDPOINT_GRANT, (uintptr_t)child, (uintptr_t)endpoint, NOS_RIGHT_RECV) &&
        !nos_syscall(NOS_SYS_MUTEX_LOCK, (uintptr_t)cycle_lock, 0, 0);
    message = (struct nos_message){ .op = 4, .arg = cycle_lock };
    ok &= !nos_syscall(NOS_SYS_CALL, (uintptr_t)endpoint, (uintptr_t)&message, (uintptr_t)&message) &&
          message.value == -NOS_EDEADLK;
    ok &= !nos_syscall(NOS_SYS_MUTEX_UNLOCK, (uintptr_t)cycle_lock, 0, 0);
    message.op = 99;
    ok &= !nos_syscall(NOS_SYS_CALL, (uintptr_t)endpoint, (uintptr_t)&message, (uintptr_t)&message) && wait_for(child, 57);
    ok &= !nos_syscall(NOS_SYS_MUTEX_DESTROY, (uintptr_t)cycle_lock, 0, 0);
    nos_check(ok, "mixed_ipc_mutex_deadlock_detection");

    struct lazy_mutex_probe probe = { .result = -1 };
    intptr_t stack = nos_syscall(NOS_SYS_MMAP, 3, 8192, 0);
    ok = !nos_pointer_is_error(stack);
    if (ok) {
        struct nos_thread_args args = { .entry = (uintptr_t)lazy_mutex_thread,
            .arg = (uintptr_t)&probe, .stack = (uintptr_t)stack, .stack_size = 8192,
            .clear_tid = (uintptr_t)&probe.tid, .parent_tid = (uintptr_t)&probe.tid };
        child = nos_syscall(NOS_SYS_THREAD_CREATE, (uintptr_t)&args, UINTPTR_MAX, 0);
        ok &= child >= 0;
        if (child >= 0) {
            probe.lock = nos_syscall(NOS_SYS_MUTEX_CREATE, 0, 0, 0);
            __atomic_store_n(&probe.gate, 1, __ATOMIC_RELEASE);
            nos_syscall(NOS_SYS_FUTEX_WAKE, (uintptr_t)&probe.gate, 1, 0);
            unsigned tid;
            while ((tid = __atomic_load_n(&probe.tid, __ATOMIC_ACQUIRE)))
                nos_syscall(NOS_SYS_FUTEX_WAIT, (uintptr_t)&probe.tid, tid, 0);
            ok &= !probe.result && !nos_syscall(NOS_SYS_MUTEX_DESTROY, (uintptr_t)probe.lock, 0, 0);
        }
        ok &= !nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)stack, 8192, 0);
    }
    nos_check(ok, "shared_thread_lazy_mutex_capability");

    endpoint = nos_syscall(NOS_SYS_ENDPOINT_CREATE, 0, 0, 0);
    child = spawn(endpoint_service, (uintptr_t)endpoint, 6);
    ok = endpoint >= 0 && child >= 0 && !nos_syscall(NOS_SYS_ENDPOINT_GRANT,
        (uintptr_t)child, (uintptr_t)endpoint, NOS_RIGHT_RECV);
    intptr_t reply_page = nos_syscall(NOS_SYS_MMAP, 3, NOS_PAGE_SIZE, 0);
    stack = nos_syscall(NOS_SYS_MMAP, 3, 8192, 0);
    ok &= !nos_pointer_is_error(reply_page) && !nos_pointer_is_error(stack);
    if (ok) {
        struct reply_unmap_probe unmap = { .address = (uintptr_t)reply_page };
        struct nos_thread_args args = { .entry = (uintptr_t)reply_unmap_thread,
            .arg = (uintptr_t)&unmap, .stack = (uintptr_t)stack, .stack_size = 8192,
            .clear_tid = (uintptr_t)&unmap.tid, .parent_tid = (uintptr_t)&unmap.tid };
        ok &= nos_syscall(NOS_SYS_THREAD_CREATE, (uintptr_t)&args, UINTPTR_MAX, 0) >= 0;
        message = (struct nos_message){ .op = 3 };
        ok &= nos_syscall(NOS_SYS_CALL, (uintptr_t)endpoint, (uintptr_t)&message,
                          (uintptr_t)reply_page) == -NOS_EFAULT;
        unsigned tid;
        while ((tid = __atomic_load_n(&unmap.tid, __ATOMIC_ACQUIRE)))
            nos_syscall(NOS_SYS_FUTEX_WAIT, (uintptr_t)&unmap.tid, tid, 0);
        message.op = 99;
        ok &= !nos_syscall(NOS_SYS_CALL, (uintptr_t)endpoint, (uintptr_t)&message, (uintptr_t)&message);
        ok &= wait_for(child, 57) && !nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)stack, 8192, 0);
    }
    nos_check(ok, "ipc_invalid_reply_buffer_completion");

    nos_syscall(NOS_SYS_OBJECT_STATS, (uintptr_t)&before, 0, 0);
    ok = 1;
    intptr_t stale_endpoint = -1;
    for (unsigned iteration = 0; iteration < 1000; iteration++) {
        endpoint = nos_syscall(NOS_SYS_ENDPOINT_CREATE, 0, 0, 0);
        if (endpoint < 0 || nos_syscall(NOS_SYS_ENDPOINT_CLOSE, (uintptr_t)endpoint, 0, 0)) { ok = 0; break; }
        stale_endpoint = endpoint;
    }
    nos_syscall(NOS_SYS_OBJECT_STATS, (uintptr_t)&after, 0, 0);
    ok &= before.endpoints == after.endpoints &&
          nos_syscall(NOS_SYS_RECV, (uintptr_t)stale_endpoint, (uintptr_t)&message, 0) == -NOS_EACCES;
    nos_check(ok, "endpoint_1000_lifecycle_cycles");

    intptr_t a = nos_syscall(NOS_SYS_MUTEX_CREATE, 0, 0, 0);
    intptr_t b = nos_syscall(NOS_SYS_MUTEX_CREATE, 0, 0, 0);
    ok = a >= 0 && b >= 0 && !nos_syscall(NOS_SYS_MUTEX_LOCK, (uintptr_t)a, 0, 0) &&
         !nos_syscall(NOS_SYS_MUTEX_LOCK, (uintptr_t)b, 0, 0);
    child = spawn(lock_chain_worker, (uintptr_t)a, 6);
    intptr_t waiter = spawn(lock_waiter, (uintptr_t)b, 4);
    nos_syscall(NOS_SYS_SLEEP, 2, 0, 0);
    ok &= nos_syscall(NOS_SYS_PRIORITY, 0, 0, 0) == 4;
    nos_syscall(NOS_SYS_MUTEX_UNLOCK, (uintptr_t)a, 0, 0);
    ok &= nos_syscall(NOS_SYS_PRIORITY, 0, 0, 0) == 4;
    nos_syscall(NOS_SYS_MUTEX_UNLOCK, (uintptr_t)b, 0, 0);
    ok &= wait_for(child, 33) && wait_for(waiter, 34);
    ok &= !nos_syscall(NOS_SYS_MUTEX_DESTROY, (uintptr_t)a, 0, 0) && !nos_syscall(NOS_SYS_MUTEX_DESTROY, (uintptr_t)b, 0, 0);
    nos_check(ok, "multi_lock_priority_inheritance");

    a = nos_syscall(NOS_SYS_MUTEX_CREATE, 0, 0, 0);
    b = nos_syscall(NOS_SYS_MUTEX_CREATE, 0, 0, 0);
    endpoint = nos_syscall(NOS_SYS_ENDPOINT_CREATE, 0, 0, 0);
    ok = a >= 0 && b >= 0 && endpoint >= 0 && !nos_syscall(NOS_SYS_MUTEX_LOCK, (uintptr_t)a, 0, 0);
    child = spawn(transitive_worker, (uintptr_t)endpoint, 6);
    ok &= child >= 0 && !nos_syscall(NOS_SYS_ENDPOINT_GRANT, (uintptr_t)child, (uintptr_t)endpoint, NOS_RIGHT_RECV);
    message = (struct nos_message){ .op = 1, .value = a, .arg = b };
    ok &= !nos_syscall(NOS_SYS_CALL, (uintptr_t)endpoint, (uintptr_t)&message, (uintptr_t)&message);
    waiter = spawn(lock_waiter, (uintptr_t)b, 4);
    /* SMP may run spawn's child concurrently; wait until donation is visible. */
    for (unsigned tries = 0; tries < 10 && nos_syscall(NOS_SYS_PRIORITY, 0, 0, 0) != 4; tries++)
        nos_syscall(NOS_SYS_SLEEP, 1, 0, 0);
    ok &= nos_syscall(NOS_SYS_PRIORITY, 0, 0, 0) == 4;
    nos_syscall(NOS_SYS_MUTEX_UNLOCK, (uintptr_t)a, 0, 0);
    ok &= wait_for(child, 45) && wait_for(waiter, 34);
    ok &= !nos_syscall(NOS_SYS_MUTEX_DESTROY, (uintptr_t)a, 0, 0) &&
          !nos_syscall(NOS_SYS_MUTEX_DESTROY, (uintptr_t)b, 0, 0);
    nos_check(ok, "transitive_priority_inheritance");

    nos_syscall(NOS_SYS_OBJECT_STATS, (uintptr_t)&before, 0, 0);
    child = spawn(orphan_parent, 0, 6);
    ok = child >= 0 && !nos_syscall(NOS_SYS_TASK_AUTHORIZE, (uintptr_t)child, NOS_CAP_SPAWN, 0);
    ok &= wait_for(child, 60);
    nos_syscall(NOS_SYS_SLEEP, 5, 0, 0);
    nos_syscall(NOS_SYS_OBJECT_STATS, (uintptr_t)&after, 0, 0);
    nos_check(ok && before.tasks == after.tasks && before.free_pages == after.free_pages,
              "orphan_task_reclamation");
#ifdef NOS_LINUX_COMPAT
    bulk_regression();
#endif

    /* Killing a service returns errors to clients; explicit init restart creates a fresh generation. */
    ok = !nos_syscall(NOS_SYS_TASK_CANCEL, 0, 0, 0);
    intptr_t server = nos_syscall(NOS_SYS_SERVICE_RESTART, 0, 0, 0);
    int fd = nos_open("/proc/version", NOS_O_RDONLY);
    ok &= server > 0 && fd >= 3;
    if (fd >= 3) nos_close(fd);
    nos_check(ok, "vfs_service_restart");
}
