/* Copyright (C) 2026 NOS contributors. Actual VFS pipe state, mock transport. */
#define main nos_record_lock_host_main
#include "host_record_locks.c"
#undef main

static unsigned pipe_index;
static int reader_fd, writer_fd;
static intptr_t bulk_to(int fd, unsigned sender, const void *bytes, size_t count, unsigned flags)
{
    struct nos_message message = { .op = NOS_VFS_WRITE, .arg = fd, .value = flags,
        .bulk_in = (uintptr_t)bytes, .bulk_in_len = count };
    return invoke(1, sender, &message, 1);
}
static intptr_t bulk_write(unsigned sender, const void *bytes, size_t count, unsigned flags)
{ return bulk_to(writer_fd, sender, bytes, count, flags); }
static int create_pipe(void)
{
    struct nos_message message = { .op = NOS_VFS_PIPE };
    if (invoke(1, 1, &message, 1)) return 0;
    int pair[2]; memcpy(pair, message.data, sizeof(pair));
    reader_fd = pair[0]; writer_fd = pair[1];
    pipe_index = opened[table_for(1)->fd[writer_fd] - 1].pipe; return 1;
}
static int ring_matches(char value, size_t count)
{
    const struct pipe_buffer *pipe = &pipes[pipe_index];
    if (pipe->size != count) return 0;
    for (size_t i = 0; i < count; i++) if (pipe->bytes[(pipe->head + i) % PIPE_BYTES] != value) return 0;
    return 1;
}
static int read_bytes(char expected, size_t count)
{
    while (count) {
        size_t wanted = count < NOS_MSG_DATA ? count : NOS_MSG_DATA;
        struct nos_message message = { .op = NOS_VFS_READ, .arg = reader_fd, .len = wanted };
        if (invoke(1, 4, &message, 1) != (intptr_t)wanted) return 0;
        for (size_t i = 0; i < wanted; i++) if (message.data[i] != expected) return 0;
        count -= wanted;
    }
    return 1;
}
int main(void)
{
    initialize();
    static char a[NOS_PIPE_BUF], b[NOS_PIPE_BUF], c[NOS_PIPE_BUF + 1];
    memset(a, 'A', sizeof(a)); memset(b, 'B', sizeof(b)); memset(c, 'C', sizeof(c));
    int ok = create_pipe() && PIPE_BYTES == 4096 && NOS_PIPE_BUF == 4096 && NOS_IPC_BULK_MAX == 4096;
    ok &= bulk_write(1, a, sizeof(a), 0) == sizeof(a) && ring_matches('A', sizeof(a));
    check(ok, "pipe_atomic_single_4096_transaction");
    ok = bulk_write(2, b, sizeof(b), 0) == HOST_PENDING;
    ok &= read_bytes('A', sizeof(a) - NOS_MSG_DATA) && !calls[2].completed && pipes[pipe_index].size == NOS_MSG_DATA;
    ok &= read_bytes('A', NOS_MSG_DATA) && calls[2].completed && calls[2].result == sizeof(b) && ring_matches('B', sizeof(b));
    ok &= read_bytes('B', sizeof(b)) && !pipes[pipe_index].size;
    check(ok, "pipe_atomic_blocked_writer_requires_entire_space");
    ok = bulk_write(1, a, sizeof(a), 0) == sizeof(a) &&
        bulk_write(2, b, sizeof(b), 0) == HOST_PENDING && bulk_write(3, c, NOS_PIPE_BUF, 0) == HOST_PENDING;
    ok &= read_bytes('A', sizeof(a)) && ring_matches('B', sizeof(b)) && !calls[3].completed;
    ok &= read_bytes('B', sizeof(b)) && ring_matches('C', NOS_PIPE_BUF) && read_bytes('C', NOS_PIPE_BUF);
    check(ok, "pipe_atomic_two_waiting_writers_do_not_interleave");
    ok = bulk_write(1, a, 1, 0) == 1;
    unsigned before_fetch = fetch_calls;
    ok &= bulk_write(2, b, sizeof(b), NOS_IO_DONTWAIT) == -NOS_EAGAIN && fetch_calls == before_fetch && ring_matches('A', 1);
    opened[table_for(1)->fd[writer_fd] - 1].flags |= NOS_O_NONBLOCK;
    ok &= bulk_write(2, b, sizeof(b), 0) == -NOS_EAGAIN && ring_matches('A', 1);
    opened[table_for(1)->fd[writer_fd] - 1].flags &= ~NOS_O_NONBLOCK;
    ok &= read_bytes('A', 1);
    check(ok, "pipe_atomic_nonblock_insufficient_space_zero_write");
    ok = bulk_write(1, a, sizeof(a), 0) == sizeof(a) && read_bytes('A', 123);
    ok &= bulk_write(2, b, sizeof(b), NOS_IO_DONTWAIT) == -NOS_EAGAIN &&
        bulk_write(2, b, sizeof(b), NOS_IO_DONTWAIT | NOS_IO_WRITE_LARGE) == 123 && pipes[pipe_index].size == PIPE_BYTES;
    ok &= read_bytes('A', NOS_PIPE_BUF - 123) && read_bytes('B', 123);
    ok &= bulk_write(1, a, sizeof(a), 0) == sizeof(a) && read_bytes('A', 17);
    struct nos_message inline_large = { .op = NOS_VFS_WRITE, .arg = writer_fd,
        .value = NOS_IO_DONTWAIT | NOS_IO_WRITE_LARGE, .len = NOS_MSG_DATA };
    memset(inline_large.data, 'B', sizeof(inline_large.data)); before_fetch = fetch_calls;
    ok &= invoke(1, 2, &inline_large, 1) == 17 && fetch_calls == before_fetch &&
        bulk_write(3, c, NOS_PIPE_BUF, NOS_IO_DONTWAIT | NOS_IO_WRITE_LARGE) == -NOS_EAGAIN;
    ok &= read_bytes('A', NOS_PIPE_BUF - 17) && read_bytes('B', 17);
    check(ok, "pipe_atomic_large_nonblock_can_commit_available_prefix");
    fetch_error = -NOS_EFAULT;
    ok = bulk_write(1, a, sizeof(a), 0) == -NOS_EFAULT && !pipes[pipe_index].size;
    fetch_error = 0; fetch_cancel = 1;
    /* The mock client disappears at FETCH; neither XFER nor REPLY may reach
     * it. Actual kernel EINTR/timeout delivery is a separate guest check. */
    ok &= bulk_write(1, a, sizeof(a), 0) == HOST_PENDING && !calls[1].active && !pipes[pipe_index].size;
    fetch_cancel = 0; fail_reply = sequence + 1;
    ok &= bulk_write(1, a, sizeof(a), 0) == -NOS_EFAULT && !pipes[pipe_index].size;
    fail_reply = sequence + 1;
    struct nos_message inline_write = { .op = NOS_VFS_WRITE, .arg = writer_fd,
        .value = NOS_IO_DONTWAIT, .len = 1, .data = {'X'} };
    ok &= invoke(1, 1, &inline_write, 1) == -NOS_EFAULT && !pipes[pipe_index].size;
    fail_reply = 0;
    check(ok, "pipe_atomic_bad_fetch_token_or_reply_injects_zero_bytes");
    ok = bulk_write(1, a, sizeof(a), 0) == sizeof(a) && bulk_write(2, b, sizeof(b), 0) == HOST_PENDING;
    unsigned description = table_for(1)->fd[writer_fd] - 1, references = opened[description].refs;
    cancel(2, calls[2].token);
    ok &= (unsigned)opened[description].refs + 1u == references && read_bytes('A', sizeof(a)) && !pipes[pipe_index].size;
    check(ok, "pipe_atomic_cancel_reclaims_pin_and_injects_zero_bytes");
    ok = bulk_write(1, a, NOS_MSG_DATA, 0) == NOS_MSG_DATA;
    opened[table_for(1)->fd[reader_fd] - 1].flags |= NOS_O_NONBLOCK;
    fail_reply = sequence + 1;
    struct nos_message read_request = { .op = NOS_VFS_READ, .arg = reader_fd,
        .len = NOS_MSG_DATA, .value = NOS_IO_DONTWAIT };
    ok &= invoke(1, 2, &read_request, 1) == -NOS_EFAULT && ring_matches('A', NOS_MSG_DATA);
    fail_reply = 0; read_request = (struct nos_message){ .op = NOS_VFS_READ,
        .arg = reader_fd, .len = NOS_MSG_DATA, .value = NOS_IO_DONTWAIT };
    ok &= invoke(1, 2, &read_request, 0) == HOST_PENDING;
    cancel(2, calls[2].token);
    ok &= ring_matches('A', NOS_MSG_DATA) && read_bytes('A', NOS_MSG_DATA);
    read_request = (struct nos_message){ .op = NOS_VFS_READ, .arg = reader_fd, .len = 1 };
    ok &= invoke(1, 2, &read_request, 1) == -NOS_EAGAIN;
    read_request.value = NOS_IO_WRITE_LARGE;
    ok &= invoke(1, 2, &read_request, 1) == -NOS_EINVAL;
    opened[table_for(1)->fd[reader_fd] - 1].flags &= ~NOS_O_NONBLOCK;
    check(ok, "pipe_atomic_nonblock_read_failed_reply_or_cancel_consumes_zero");
    ok = bulk_write(1, a, NOS_MSG_DATA, 0) == NOS_MSG_DATA;
    struct { unsigned char before[8], bytes[NOS_MSG_DATA], after[8]; } output;
    memset(&output, 0xa5, sizeof(output));
    struct nos_message span_read = { .op = NOS_VFS_READ, .arg = reader_fd,
        .len = NOS_MSG_DATA, .bulk_out = (uintptr_t)output.bytes, .bulk_out_len = NOS_MSG_DATA };
    store_error = -NOS_EFAULT;
    ok &= invoke(1, 2, &span_read, 1) == -NOS_EFAULT && ring_matches('A', NOS_MSG_DATA);
    for (unsigned i = 0; i < sizeof(output.bytes); i++) ok &= output.bytes[i] == 0xa5;
    store_error = 0;
    span_read.len = NOS_MSG_DATA - 1; span_read.value = 0;
    ok &= invoke(1, 2, &span_read, 1) == -NOS_EINVAL && ring_matches('A', NOS_MSG_DATA);
    span_read.len = NOS_MSG_DATA; span_read.value = 0; span_read.bulk_out_len = NOS_MSG_DATA - 1;
    ok &= invoke(1, 2, &span_read, 1) == -NOS_EINVAL && ring_matches('A', NOS_MSG_DATA);
    span_read.len = NOS_MSG_DATA; span_read.value = 0; span_read.bulk_out_len = NOS_MSG_DATA;
    fail_reply = sequence + 1;
    ok &= invoke(1, 2, &span_read, 1) == -NOS_EFAULT && ring_matches('A', NOS_MSG_DATA);
    for (unsigned i = 0; i < sizeof(output.bytes); i++) ok &= output.bytes[i] == 'A';
    fail_reply = 0; cancel_after_store = 1;
    span_read.len = NOS_MSG_DATA; span_read.value = 0;
    ok &= invoke(1, 2, &span_read, 1) == HOST_PENDING && !calls[2].active && ring_matches('A', NOS_MSG_DATA);
    /* STORE can already have changed the application buffer. Only pipe
     * consumption is conditional on the subsequent token-checked REPLY. */
    for (unsigned i = 0; i < sizeof(output.bytes); i++) ok &= output.bytes[i] == 'A';
    cancel_after_store = 0; span_read.len = NOS_MSG_DATA; span_read.value = 0;
    ok &= invoke(1, 2, &span_read, 1) == NOS_MSG_DATA && !span_read.len && !pipes[pipe_index].size;
    for (unsigned i = 0; i < 8; i++) ok &= output.before[i] == 0xa5 && output.after[i] == 0xa5;
    check(ok, "pipe_atomic_read_span_store_and_reply_control_consumption");
    ok = bulk_write(1, c, sizeof(c), 0) == -NOS_EINVAL && !pipes[pipe_index].size;
    struct nos_message mixed = { .op = NOS_VFS_WRITE, .arg = writer_fd, .len = 1,
        .bulk_in = (uintptr_t)a, .bulk_in_len = 1 };
    ok &= invoke(1, 1, &mixed, 1) == -NOS_EINVAL && !pipes[pipe_index].size;
    check(ok, "pipe_atomic_oversize_and_mixed_payload_rejected");
    ok = bulk_write(1, a, sizeof(a), 0) == sizeof(a) && bulk_write(2, b, sizeof(b), 0) == HOST_PENDING;
    description = table_for(1)->fd[writer_fd] - 1;
    ok &= !close_file(1, 3, writer_fd, 1) && opened[description].refs == 1;
    int replacement = open_file_as(1, 3, "/tmp/reused_pipe_fd", NOS_O_CREAT | NOS_O_RDWR);
    unsigned replacement_description = table_for(1)->fd[replacement] - 1;
    ok &= replacement == writer_fd && replacement_description != description;
    ok &= read_bytes('A', sizeof(a)) && calls[2].completed && calls[2].result == sizeof(b) && ring_matches('B', sizeof(b));
    ok &= !opened[description].refs && !pipes[pipe_index].writers &&
        !nodes[opened[replacement_description].node].size && read_bytes('B', sizeof(b));
    read_request = (struct nos_message){ .op = NOS_VFS_READ, .arg = reader_fd, .len = 1 };
    ok &= !invoke(1, 4, &read_request, 1) && !close_file(1, 1, reader_fd, 1) && !close_file(1, 1, replacement, 1);
    check(ok, "pipe_atomic_close_and_fd_reuse_preserve_pinned_ofd");
    ok = create_pipe() && !close_file(1, 1, reader_fd, 1); before_fetch = fetch_calls;
    ok &= bulk_write(1, a, sizeof(a), 0) == -32 && !pipes[pipe_index].size && fetch_calls == before_fetch;
    check(ok, "pipe_atomic_epipe_without_fetch_or_commit");
    close_file(1, 1, writer_fd, 1);
    int regular = open_file(1, "/tmp/bulk_regular", NOS_O_CREAT | NOS_O_RDWR);
    description = table_for(1)->fd[regular] - 1; unsigned node = opened[description].node;
    static char regular_backing[FILE_BYTES];
    /* Host binding for the kernel shared-memory transport, not a copied FS
     * algorithm. Real shared object/mapping permissions are guest-tested. */
    file_backing[node].handle = 1; file_backing[node].address = regular_backing;
    ok = bulk_to(regular, 1, a, sizeof(a), 0) == sizeof(a) && nodes[node].size == sizeof(a) &&
        opened[description].offset == sizeof(a) && !memcmp(node_bytes(&nodes[node]), a, sizeof(a));
    uart_bytes = uart_calls = uart_max = 0;
    ok &= bulk_to(1, 1, b, sizeof(b), 0) == sizeof(b) && uart_bytes == sizeof(b) && uart_calls == 4 && uart_max == 1024;
    check(ok, "pipe_atomic_regular_bulk_and_uart_1024_chunks");
    printf("PIPE ATOMIC HOST %s (%u failures; mocked transport only)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
