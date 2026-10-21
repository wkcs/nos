/* Copyright (C) 2026 NOS contributors. Host state checks; mocked IPC is not guest validation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../servers/vfs/server.c"
#define HOST_PENDING (-1000000)
struct host_call { uint64_t token; intptr_t sender, result; unsigned active, completed; struct nos_message request, reply; struct nos_credentials credentials; };
static struct host_call calls[NOS_TASKS];
static uint64_t sequence, fail_reply;
static unsigned failures, batch_queries, scalar_queries;
static struct nos_credentials host_credentials;
static intptr_t subject_error, clock_error;
static uint64_t host_nofile_soft = NOS_FD_LIMIT, host_nofile_hard = NOS_FD_LIMIT;
static intptr_t limits_error;
static unsigned limits_mismatch, limits_cancel;
static int64_t host_seconds = 1700000000;
static unsigned host_fat_fixture, host_fat_list_secret;
static intptr_t batch_error;
static intptr_t store_error;
static unsigned store_cancel, store_calls, cancel_after_store;
static intptr_t fetch_error;
static unsigned fetch_cancel, fetch_calls;
static size_t uart_bytes, uart_calls, uart_max;
static unsigned char archive_image[1536];
static struct { uintptr_t owner; intptr_t state; unsigned used; } host_groups[NOS_TASKS + 4];
static void group_state(uintptr_t owner, intptr_t state)
{
    for (unsigned i = 0; i < NOS_TASKS + 4; i++) if (host_groups[i].used && host_groups[i].owner == owner) { host_groups[i].state = state; return; }
    for (unsigned i = 0; i < NOS_TASKS + 4; i++) if (!host_groups[i].used) { host_groups[i].owner = owner; host_groups[i].state = state; host_groups[i].used = 1; return; }
    abort();
}
static struct fd_table *existing_table(intptr_t owner)
{ for (unsigned i = 0; i < FD_TABLE_COUNT; i++) if (fds[i].pid == owner) return &fds[i]; return NULL; }
void *u_memcpy(void *out, const void *in, size_t count) { return memcpy(out, in, count); }
void *u_memset(void *out, int value, size_t count) { return memset(out, value, count); }
size_t u_strlen(const char *text) { return strlen(text); }
int u_streq(const char *a, const char *b) { return !strcmp(a, b); }
intptr_t nos_syscall(uintptr_t number, uintptr_t a, uintptr_t b, uintptr_t c)
{
    (void)c;
    if (number == NOS_SYS_IPC_SUBJECT) {
        const struct nos_ipc_subject_query *query = (void *)a;
        if (subject_error) return subject_error;
        if (query->reserved || b || c) return -NOS_EINVAL;
        struct host_call *call = &calls[query->sender % NOS_TASKS];
        if (!call->active || call->sender != query->sender || call->token != query->token) return -NOS_EPERM;
        struct nos_ipc_subject snapshot = { .token = query->token, .sender = query->sender,
            .process = call->request.process, .credentials = call->credentials };
        memcpy((void *)(uintptr_t)query->output, &snapshot, sizeof(snapshot)); return 0;
    }
    if (number == NOS_SYS_IPC_LIMITS) {
        const struct nos_ipc_subject_query *query = (void *)a;
        if (limits_error) return limits_error;
        if (query->reserved || b || c) return -NOS_EINVAL;
        struct host_call *call = &calls[query->sender % NOS_TASKS];
        if (limits_cancel) call->active = 0;
        if (!call->active || call->sender != query->sender || call->token != query->token) return -NOS_EPERM;
        struct nos_ipc_limits value = { .token = query->token, .sender = query->sender,
            .process = call->request.process, .nofile_soft = host_nofile_soft, .nofile_hard = host_nofile_hard };
        if (limits_mismatch == 1) value.token++;
        if (limits_mismatch == 2) value.sender++;
        if (limits_mismatch == 3) value.process++;
        memcpy((void *)(uintptr_t)query->output, &value, sizeof(value)); return 0;
    }
    if (number == NOS_SYS_CLOCK_GET64) {
        if (clock_error) return clock_error;
        if (a || c) return -NOS_EINVAL;
        struct nos_clock_value now = { .seconds = host_seconds };
        memcpy((void *)b, &now, sizeof(now)); return 0;
    }
    if (number == NOS_SYS_GROUP_ALIVE) {
        if (c == NOS_GROUP_QUERY_BATCH) {
            batch_queries++;
            if (batch_error) return batch_error;
            if (!b || b > NOS_GROUP_QUERY_MAX) return -NOS_EINVAL;
            struct nos_group_state *groups = (void *)a;
            for (size_t n = 0; n < b; n++) {
                groups[n].state = NOS_GROUP_LIVE_TASKS;
                for (unsigned i = 0; i < NOS_TASKS + 4; i++)
                    if (host_groups[i].used && host_groups[i].owner == groups[n].group) groups[n].state = host_groups[i].state;
            }
            return 0;
        }
        scalar_queries++;
        for (unsigned i = 0; i < NOS_TASKS + 4; i++) if (host_groups[i].used && host_groups[i].owner == a) return host_groups[i].state;
        return 1;
    }
    if (number == NOS_SYS_REPLY) {
        struct nos_message *reply = (void *)b; struct host_call *call = &calls[a % NOS_TASKS];
        if (!call->active || call->sender != (intptr_t)a || call->token != reply->token) return -NOS_EPERM;
        call->active = 0; call->completed = 1; call->reply = *reply;
        call->result = reply->token == fail_reply ? -NOS_EFAULT : reply->value;
        return reply->token == fail_reply ? -NOS_EFAULT : 0;
    }
    if (number == NOS_SYS_IPC_XFER) {
        const struct nos_ipc_xfer *transfer = (void *)a;
        struct host_call *call = &calls[(uintptr_t)transfer->sender % NOS_TASKS];
        if (transfer->direction > NOS_IPC_STORE) return -NOS_EINVAL;
        int store = transfer->direction == NOS_IPC_STORE;
        if (store ? store_cancel : fetch_cancel) call->active = 0;
        if (!call->active || call->sender != transfer->sender || call->token != transfer->token) return -NOS_EPERM;
        size_t length = store ? call->request.bulk_out_len : call->request.bulk_in_len;
        uintptr_t base = store ? call->request.bulk_out : call->request.bulk_in;
        if (transfer->offset > length || transfer->count > length - transfer->offset) return -NOS_EINVAL;
        if (store) {
            store_calls++; if (store_error) return store_error;
            memcpy((void *)(base + transfer->offset), (void *)transfer->buffer, transfer->count);
            if (cancel_after_store) call->active = 0;
        } else {
            fetch_calls++; if (fetch_error) return fetch_error;
            memcpy((void *)transfer->buffer, (void *)(base + transfer->offset), transfer->count);
        }
        return 0;
    }
    if (number == NOS_SYS_WRITE) {
        if (b > 1024) return -NOS_EINVAL;
        uart_calls++; uart_bytes += b; if (b > uart_max) uart_max = b;
        return (intptr_t)b;
    }
    if (number == NOS_SYS_BOOTFILE) {
        struct nos_bootfile_io *io = (void *)b;
        if (a != NOS_BOOTFILE_READ || io->index != 77) return -NOS_ENOENT;
        if (io->offset >= sizeof(archive_image)) return 0;
        size_t count = io->count;
        if (count > sizeof(archive_image) - io->offset) count = sizeof(archive_image) - (size_t)io->offset;
        memcpy((void *)io->buffer, archive_image + (size_t)io->offset, count); return (intptr_t)count;
    }
    return -NOS_ENOTSUP;
}
void nos_uart_init(void) { }
int nos_uart_readable(void) { return 0; }
int nos_uart_writable(void) { return 1; }
void nos_uart_get_stats(struct nos_uart_stats *s) { memset(s, 0, sizeof(*s)); }
intptr_t nos_random_read(void *buffer, size_t count, unsigned flags) { (void)buffer; (void)count; (void)flags; return -NOS_ENOTSUP; }
uint64_t nos_block_capacity(void) { return 0; }
uint64_t nos_block_interrupts(void) { return 0; }
uint64_t nos_block_requests(void) { return 0; }
uint64_t nos_block_poll_fallbacks(void) { return 0; }
unsigned nos_block_version(void) { return 0; }
intptr_t nos_device_uart_read(void) { return -NOS_EAGAIN; }
int nos_block_init(void) { return -NOS_ENOTSUP; }
int nos_block_read(unsigned n,void *b) { (void)n;(void)b;return -NOS_ENOTSUP; }
int nos_block_write(unsigned n,const void *b) { (void)n;(void)b;return -NOS_ENOTSUP; }
int nos_block_flush(void) { return -NOS_ENOTSUP; }
int nos_fat_mount(int n) { (void)n;return host_fat_fixture ? 0 : -NOS_ENOTSUP; }
int nos_fat_list(const char *p,unsigned i,char *n,unsigned *m,size_t *s)
{
    if (!host_fat_fixture) return -NOS_ENOTSUP;
    if (!host_fat_list_secret || strcmp(p, "/") || i) return 0;
    strcpy(n, "SECRET"); *m = MODE_FILE; *s = 1; return 1;
}
int nos_fat_read(const char *p,void *b,size_t n) { (void)p;if (!host_fat_fixture) return -NOS_ENOTSUP;if (n) *(char *)b = 'S';return (int)n; }
int nos_fat_write(const char *p,const void *b,size_t n) { (void)p;(void)b;(void)n;return host_fat_fixture ? 0 : -NOS_ENOTSUP; }
int nos_fat_mkdir(const char *p) { (void)p;return -NOS_ENOTSUP; }
int nos_fat_unlink(const char *p) { (void)p;return -NOS_ENOTSUP; }
int nos_fat_rename(const char *p,const char *n) { (void)p;(void)n;return -NOS_ENOTSUP; }
int nos_fat_sync(void) { return -NOS_ENOTSUP; }
static void check(int condition, const char *name)
{ printf("%s %s\n", condition ? "PASS" : "FAIL", name); failures += !condition; }
static intptr_t invoke(unsigned owner, unsigned sender, struct nos_message *message, int finish)
{
    message->sender = sender; message->process = owner; message->token = ++sequence;
    struct host_call *call = &calls[sender % NOS_TASKS];
    *call = (struct host_call){ .sender = sender, .token = sequence, .active = 1, .request = *message, .credentials = host_credentials };
    defer_reply = 0; service(message);
    if (!defer_reply) { call->active = 0; call->completed = 1; call->result = message->value; call->reply = *message; }
    if (finish) service_complete();
    if (call->completed) { *message = call->reply; return call->result; }
    return HOST_PENDING;
}
static int open_file_as(unsigned owner, unsigned sender, const char *name, unsigned flags)
{
    struct nos_message message = { .op = NOS_VFS_OPEN, .arg = flags, .len = strlen(name) + 1 };
    strcpy(message.data, name); return (int)invoke(owner, sender, &message, 1);
}
static int open_file(unsigned owner, const char *name, unsigned flags)
{ return open_file_as(owner, owner, name, flags); }
static intptr_t change_lock(unsigned owner, unsigned sender, int fd, unsigned command, int type,
                            int64_t start, int64_t length, int whence, int finish, struct nos_record_lock *result)
{
    struct nos_message message = { .op = NOS_VFS_RECORD_LOCK, .arg = fd, .value = command, .len = sizeof(struct nos_record_lock) };
    struct nos_record_lock wire = { .start = start, .length = length, .pid = 999, .type = (int16_t)type, .whence = (int16_t)whence };
    memcpy(message.data, &wire, sizeof(wire));
    intptr_t rc = invoke(owner, sender, &message, finish);
    if (!rc && result) memcpy(result, message.data, sizeof(*result));
    return rc;
}
static intptr_t lock_set(unsigned owner, int fd, int type, int64_t start, int64_t length)
{ return change_lock(owner, owner, fd, NOS_LOCK_SET, type, start, length, 0, 1, NULL); }
static intptr_t close_file(unsigned owner, unsigned sender, int fd, int finish)
{ struct nos_message message = { .op = NOS_VFS_CLOSE, .arg = fd }; return invoke(owner, sender, &message, finish); }
static int duplicate(unsigned owner, int fd, int target)
{ struct nos_message message = { .op = NOS_VFS_DUP, .arg = fd, .value = target }; return (int)invoke(owner, owner, &message, 1); }
static unsigned waits(void)
{ unsigned count = 0; for (unsigned i = 0; i < NOS_TASKS; i++) count += pending_io[i].used == RECORD_WAIT_KIND; return count; }
static void cancel(unsigned sender, uint64_t token)
{ calls[sender % NOS_TASKS].active = 0; pipe_cancel(sender, token, 1); service_complete(); }
static void initialize(void)
{ initialize_namespace(); }
static intptr_t bulk_pread(int fd, uint64_t offset, void *buffer, size_t count, size_t metadata)
{
    struct nos_message message = { .op = NOS_VFS_PREAD, .arg = fd, .len = metadata,
        .bulk_out = (uintptr_t)buffer, .bulk_out_len = count };
    memcpy(message.data, &offset, sizeof(offset));
    intptr_t rc = invoke(1, 1, &message, 1);
    if (rc >= 0 && message.len) return -NOS_EIO;
    return rc;
}
static void bulk_pread_checks(void)
{
    int fd = open_file(1, "/tmp/bulkpread", NOS_O_CREAT | NOS_O_RDWR);
    struct description *description = &opened[table_for(1)->fd[fd] - 1];
    struct vnode *node = &nodes[description->node];
    for (unsigned i = 0; i < sizeof(node->bytes); i++) node->bytes[i] = (char)(i * 17u);
    node->size = sizeof(node->bytes); description->offset = 137;
    struct { unsigned char before[8], bytes[512], after[8]; } output;
    memset(&output, 0xa5, sizeof(output));
    unsigned transfers = store_calls;
    int ok = bulk_pread(fd, 0, output.bytes, sizeof(output.bytes), 8) == 512 &&
        !memcmp(output.bytes, node->bytes, 512) && description->offset == 137 && store_calls == transfers + 1;
    for (unsigned i = 0; i < 8; i++) ok &= output.before[i] == 0xa5 && output.after[i] == 0xa5;
    struct nos_message inline_read = { .op = NOS_VFS_PREAD, .arg = fd, .len = 32 };
    uint64_t offset = 19; memcpy(inline_read.data, &offset, sizeof(offset));
    ok &= invoke(1, 1, &inline_read, 1) == 32 && inline_read.len == 32 &&
        !memcmp(inline_read.data, node->bytes + 19, 32) && description->offset == 137;
    check(ok, "vfs_bulk_pread_ram_inline_and_offset_preserved");
    unsigned d = (unsigned)free_description(); int archive_fd = free_fd(table_for(1));
    opened[d] = (struct description){ .refs = 1, .kind = ARCHIVE_FILE, .node = 77, .offset = 91, .size = sizeof(archive_image) };
    table_for(1)->fd[archive_fd] = d + 1;
    for (unsigned i = 0; i < sizeof(archive_image); i++) archive_image[i] = (unsigned char)(i * 23u + 3u);
    memset(&output, 0xa5, sizeof(output));
    ok = bulk_pread(archive_fd, 1000, output.bytes, 512, 8) == 512 && !memcmp(output.bytes, archive_image + 1000, 512);
    memset(output.bytes, 0xa5, sizeof(output.bytes));
    ok &= bulk_pread(archive_fd, 1500, output.bytes, 512, 8) == 36 && !memcmp(output.bytes, archive_image + 1500, 36);
    for (unsigned i = 36; i < sizeof(output.bytes); i++) ok &= output.bytes[i] == 0xa5;
    transfers = store_calls;
    ok &= !bulk_pread(archive_fd, 1536, output.bytes, 512, 8) && store_calls == transfers && opened[d].offset == 91;
    check(ok, "vfs_bulk_pread_archive_short_read_and_eof");
    memset(output.bytes, 0xa5, sizeof(output.bytes)); store_error = -NOS_EFAULT;
    ok = bulk_pread(fd, 0, output.bytes, 512, 8) == -NOS_EFAULT && description->offset == 137;
    for (unsigned i = 0; i < sizeof(output.bytes); i++) ok &= output.bytes[i] == 0xa5;
    store_error = 0; store_cancel = 1;
    ok &= bulk_pread(fd, 0, output.bytes, 512, 8) == -NOS_EPERM && description->offset == 137;
    for (unsigned i = 0; i < sizeof(output.bytes); i++) ok &= output.bytes[i] == 0xa5;
    store_cancel = 0;
    check(ok, "vfs_bulk_pread_store_failure_and_cancel_preserve_state");
    transfers = store_calls;
    ok = bulk_pread(fd, 0, output.bytes, 512, 0) == -NOS_EINVAL &&
        bulk_pread(fd, 0, output.bytes, 512, 7) == -NOS_EINVAL &&
        bulk_pread(fd, 0, output.bytes, 512, 9) == -NOS_EINVAL &&
        bulk_pread(fd, 0, output.bytes, 513, 8) == -NOS_EINVAL &&
        bulk_pread(fd, UINT64_MAX, output.bytes, 512, 8) == -NOS_EINVAL &&
        bulk_pread(FDS, 0, output.bytes, 512, 8) == -NOS_EBADF && store_calls == transfers;
    ok &= !bulk_pread(fd, INT64_MAX, output.bytes, 512, 8) && description->offset == 137 &&
        bulk_pread(0, 0, output.bytes, 512, 8) == -29 && store_calls == transfers;
    int directory_fd = open_file(1, "/tmp", NOS_O_DIRECTORY);
    ok &= directory_fd >= 3 && bulk_pread(directory_fd, 0, output.bytes, 512, 8) == -NOS_EISDIR && store_calls == transfers;
    if (directory_fd >= 0) close_file(1, 1, directory_fd, 1);
    unsigned old_flags = description->flags; description->flags = NOS_O_WRONLY;
    ok &= bulk_pread(fd, 0, output.bytes, 512, 8) == -NOS_EBADF && store_calls == transfers && description->offset == 137;
    description->flags = old_flags;
    check(ok, "vfs_bulk_pread_format_permissions_and_bounds");
    close_file(1, 1, archive_fd, 1); close_file(1, 1, fd, 1);
    struct nos_message remove = { .op = NOS_VFS_UNLINK, .len = 15 };
    strcpy(remove.data, "/tmp/bulkpread"); remove.len = strlen(remove.data) + 1; invoke(1, 1, &remove, 1);
}
/* Use actual service/defer/GC functions, with the harness's mock transport. */
static unsigned pipe_wait_count(void)
{
    unsigned count = 0;
    for (unsigned i = 0; i < NOS_TASKS; i++) count += pending_io[i].used == 1;
    return count;
}
static int gc_pipe_pair(unsigned parent, unsigned child, int pair[2])
{
    group_state(parent, NOS_GROUP_LIVE_TASKS); group_state(child, NOS_GROUP_LIVE_TASKS);
    struct nos_message request = { .op = NOS_VFS_PIPE };
    if (invoke(parent, parent, &request, 1)) return 0;
    memcpy(pair, request.data, sizeof(int) * 2);
    return !inherit_files(parent, child);
}
static intptr_t gc_pipe_io(unsigned owner, unsigned sender, unsigned op, int fd, size_t count, char value)
{
    struct nos_message request = { .op = op, .arg = fd, .len = count };
    memset(request.data, value, count);
    intptr_t result = invoke(owner, sender, &request, 1);
    if (result > 0 && op == NOS_VFS_READ)
        for (intptr_t i = 0; i < result; i++) if (request.data[i] != value) return -NOS_EIO;
    return result;
}
static void pipe_gc_checks(void)
{
    int pair[2]; int ok = gc_pipe_pair(4, 5, pair);
    ok &= !close_file(4, 4, pair[1], 1) && !close_file(5, 5, pair[0], 1);
    unsigned reader = table_for(4)->fd[pair[0]] - 1, pipe = opened[reader].pipe;
    ok &= gc_pipe_io(4, 4, NOS_VFS_READ, pair[0], 1, 'A') == HOST_PENDING && opened[reader].refs == 2;
    /* No PEER_EXIT or PEER_CANCEL notice is delivered. A failed query must not
     * destroy a live or unknown owner's pending request. */
    group_state(4, -NOS_EACCES); service_complete();
    ok &= pipe_wait_count() == 1 && opened[reader].refs == 2;
    /* A dead parent still referenced by FORK retains only its FD inheritance
     * state: its obsolete syscall pin is removed before the FD can be freed. */
    group_state(4, NOS_GROUP_FORK_REFERENCES); service_complete();
    ok &= existing_table(4) && !pipe_wait_count() && opened[reader].refs == 1 && pipes[pipe].readers == 1;
    group_state(4, 0); service_complete();
    ok &= !existing_table(4) && !pipe_wait_count() && !pipes[pipe].readers &&
        gc_pipe_io(5, 5, NOS_VFS_WRITE, pair[1], 1, 'B') == -32;
    group_state(5, 0); service_complete();
    check(ok, "vfs_pending_owner_gc_lost_reader_exit_unpins_epipe");

    ok = gc_pipe_pair(4, 5, pair);
    ok &= !close_file(4, 4, pair[0], 1) && !close_file(5, 5, pair[1], 1);
    unsigned writer = table_for(4)->fd[pair[1]] - 1; pipe = opened[writer].pipe;
    for (unsigned i = 0; i < PIPE_BYTES / NOS_MSG_DATA; i++)
        ok &= gc_pipe_io(4, 4, NOS_VFS_WRITE, pair[1], NOS_MSG_DATA, 'A') == NOS_MSG_DATA;
    ok &= gc_pipe_io(4, 4, NOS_VFS_WRITE, pair[1], NOS_MSG_DATA, 'B') == HOST_PENDING && opened[writer].refs == 2;
    group_state(4, 0); service_complete();
    ok &= !existing_table(4) && !pipe_wait_count() && !pipes[pipe].writers && pipes[pipe].size == PIPE_BYTES;
    for (unsigned i = 0; i < PIPE_BYTES / NOS_MSG_DATA; i++)
        ok &= gc_pipe_io(5, 5, NOS_VFS_READ, pair[0], NOS_MSG_DATA, 'A') == NOS_MSG_DATA;
    ok &= !gc_pipe_io(5, 5, NOS_VFS_READ, pair[0], 1, 'A') && !pipes[pipe].size;
    group_state(5, 0); service_complete();
    check(ok, "vfs_pending_owner_gc_lost_writer_exit_preserves_data_and_eof");

    ok = gc_pipe_pair(4, 5, pair);
    ok &= !inherit_files(4, 16);
    group_state(16, NOS_GROUP_LIVE_TASKS);
    ok &= !close_file(4, 4, pair[1], 1) && !close_file(16, 16, pair[1], 1) &&
        !close_file(5, 5, pair[0], 1);
    /* owner4 and owner16 reuse the same task slot, but only owner4 is dead.
     * Use separate sender slots to keep both mock calls outstanding. */
    ok &= gc_pipe_io(4, 4, NOS_VFS_READ, pair[0], 1, 'A') == HOST_PENDING &&
        gc_pipe_io(16, 6, NOS_VFS_READ, pair[0], 1, 'A') == HOST_PENDING;
    group_state(4, 0); service_complete();
    ok &= !existing_table(4) && existing_table(16) && pipe_wait_count() == 1 &&
        calls[6].active && !calls[6].completed;
    /* A different thread's exit in the still-live group must cancel only its
     * own sender. It must not invoke the process-wide cleanup path. */
    pipe_cancel(7, 0, 0); service_complete();
    ok &= pipe_wait_count() == 1 && calls[6].active;
    ok &= gc_pipe_io(5, 5, NOS_VFS_WRITE, pair[1], 1, 'A') == 1 &&
        calls[6].completed && calls[6].result == 1 && !pipe_wait_count();
    group_state(5, 0); group_state(16, 0); service_complete();
    group_state(4, NOS_GROUP_LIVE_TASKS); group_state(5, NOS_GROUP_LIVE_TASKS);
    check(ok, "vfs_pending_owner_gc_full_generation_and_live_thread_isolation");
}

int main(void)
{
    initialize();
    bulk_pread_checks();
    pipe_gc_checks();
    int a = open_file(1, "/tmp/a", NOS_O_CREAT | NOS_O_RDWR), b = open_file(2, "/tmp/a", NOS_O_RDWR);
    int c = open_file(3, "/tmp/a", NOS_O_RDWR); struct nos_record_lock wire;
    int ok = a >= 3 && b >= 3 && c >= 3 && !lock_set(1, a, NOS_LOCK_WRITE, 10, 10);
    ok &= !change_lock(2, 2, b, NOS_LOCK_GET, NOS_LOCK_WRITE, 0, 0, 0, 1, &wire) && wire.pid == 1 && wire.type == NOS_LOCK_WRITE && wire.start == 10 && wire.length == 10 && wire.whence == 0;
    ok &= !lock_set(1, a, NOS_LOCK_READ, 0, 20) && !lock_set(2, b, NOS_LOCK_READ, 2, 5) && lock_set(3, c, NOS_LOCK_WRITE, 2, 1) == -NOS_EAGAIN;
    check(ok, "record_locks_process_conflicts_and_getlk");
    ok = !lock_set(1, a, NOS_LOCK_UNLOCK, 4, 2);
    ok &= !change_lock(3, 3, c, NOS_LOCK_GET, NOS_LOCK_WRITE, 0, 1, 0, 1, &wire) && wire.start == 0 && wire.length == 4;
    ok &= !lock_set(1, a, NOS_LOCK_READ, 4, 2);
    unsigned own = 0; for (unsigned i = 0; i < record_lock_count; i++) own += record_locks[i].owner == 1;
    ok &= own == 1 && !close_file(2, 2, b, 1) && !lock_set(1, a, NOS_LOCK_UNLOCK, 0, 0);
    struct description *file = &opened[table_for(1)->fd[a] - 1]; file->offset = 10;
    ok &= !change_lock(1, 1, a, NOS_LOCK_SET, NOS_LOCK_WRITE, -2, -3, 1, 1, NULL);
    ok &= !change_lock(3, 3, c, NOS_LOCK_GET, NOS_LOCK_WRITE, 5, 1, 0, 1, &wire) && wire.start == 5 && wire.length == 3;
    ok &= change_lock(1, 1, a, NOS_LOCK_SET, NOS_LOCK_WRITE, -1, 1, 0, 1, NULL) == -NOS_EINVAL;
    ok &= change_lock(1, 1, a, NOS_LOCK_SET, NOS_LOCK_WRITE, INT64_MAX, 2, 0, 1, NULL) == -NOS_EOVERFLOW;
    check(ok, "record_locks_split_merge_negative_len_checked_offsets");
    int extra = open_file(1, "/tmp/a", NOS_O_RDWR);
    ok = extra >= 3 && !close_file(1, 1, extra, 1) && !record_lock_count && !lock_set(1, a, NOS_LOCK_WRITE, 0, 0);
    int dup = duplicate(1, a, -1); ok &= dup >= 3 && !close_file(1, 1, dup, 1) && !record_lock_count;
    ok &= !lock_set(1, a, NOS_LOCK_WRITE, 0, 0);
    extra = open_file(1, "/tmp/a", NOS_O_RDWR); int other = open_file(1, "/tmp/other", NOS_O_CREAT | NOS_O_RDWR);
    ok &= extra >= 3 && other >= 3 && duplicate(1, other, extra) == extra && !record_lock_count;
    check(ok, "record_locks_any_close_and_dup2_target_release");
    ok = !lock_set(1, a, NOS_LOCK_WRITE, 0, 0); exec_files(1); ok &= record_lock_count == 1;
    extra = open_file(1, "/tmp/a", NOS_O_RDWR | NOS_O_CLOEXEC); ok &= extra >= 3;
    exec_files(1); ok &= !record_lock_count && table_for(1)->fd[a];
    ok &= !lock_set(1, a, NOS_LOCK_WRITE, 0, 0);
    release_table(table_for(3)); inherit_files(1, 3);
    ok &= change_lock(3, 3, a, NOS_LOCK_SET, NOS_LOCK_READ, 0, 1, 0, 1, NULL) == -NOS_EAGAIN;
    ok &= !close_file(3, 3, a, 1) && record_lock_count == 1;
    record_lock_owner_exit(3); release_table(table_for(3));
    check(ok, "record_locks_fork_no_lock_inheritance_exec_cloexec");
    lock_set(1, a, NOS_LOCK_UNLOCK, 0, 0);
    b = open_file(2, "/tmp/a", NOS_O_RDWR); int second = open_file(2, "/tmp/other", NOS_O_RDWR);
    ok = b >= 3 && second >= 3 && !lock_set(1, a, NOS_LOCK_WRITE, 0, 0) && !lock_set(2, second, NOS_LOCK_WRITE, 0, 0);
    ok &= change_lock(1, 1, other, NOS_LOCK_SET_WAIT, NOS_LOCK_WRITE, 0, 0, 0, 1, NULL) == HOST_PENDING && waits() == 1;
    ok &= change_lock(2, 2, b, NOS_LOCK_SET_WAIT, NOS_LOCK_WRITE, 0, 0, 0, 1, NULL) == -NOS_EDEADLK;
    ok &= !lock_set(2, second, NOS_LOCK_UNLOCK, 0, 0) && calls[1].completed && !calls[1].result && !waits();
    lock_set(1, a, NOS_LOCK_UNLOCK, 0, 0); lock_set(1, other, NOS_LOCK_UNLOCK, 0, 0);
    check(ok, "record_locks_wait_deadlock_and_unlock_wakeup");
    ok = !lock_set(2, b, NOS_LOCK_WRITE, 0, 0);
    unsigned description = table_for(1)->fd[a] - 1, references = opened[description].refs;
    ok &= change_lock(1, 1, a, NOS_LOCK_SET_WAIT, NOS_LOCK_WRITE, 0, 0, 0, 1, NULL) == HOST_PENDING;
    uint64_t token = calls[1].token; ok &= opened[description].refs == references + 1;
    pipe_cancel(1, token - 1, 1); ok &= waits() == 1;
    cancel(1, token); ok &= !waits() && opened[description].refs == references;
    ok &= !lock_set(2, b, NOS_LOCK_UNLOCK, 0, 0) && !record_lock_count;
    check(ok, "record_locks_cancel_reclaims_pin_without_ghost_lock");
    ok = !lock_set(2, b, NOS_LOCK_WRITE, 0, 0);
    ok &= change_lock(1, 1, a, NOS_LOCK_SET_WAIT, NOS_LOCK_WRITE, 0, 0, 0, 1, NULL) == HOST_PENDING;
    ok &= !close_file(1, 11, a, 0); int reused = open_file_as(1, 11, "/tmp/other", NOS_O_RDWR);
    ok &= reused == a && !waits() && calls[1].completed && calls[1].result == -NOS_EBADF;
    lock_set(2, b, NOS_LOCK_UNLOCK, 0, 0);
    check(ok, "record_locks_close_fd_reuse_cannot_redirect_waiter");
    a = open_file(1, "/tmp/a", NOS_O_RDWR); references = opened[table_for(1)->fd[a] - 1].refs;
    fail_reply = sequence + 1;
    ok = lock_set(1, a, NOS_LOCK_WRITE, 0, 0) == -NOS_EFAULT && !record_lock_count && !waits() && opened[table_for(1)->fd[a] - 1].refs == references;
    fail_reply = 0; check(ok, "record_locks_failed_reply_does_not_commit");
    uint64_t inode; record_inode(&opened[table_for(1)->fd[a] - 1], &inode);
    record_lock_count = RECORD_LOCK_LIMIT;
    for (unsigned i = 0; i < RECORD_LOCK_LIMIT; i++) record_locks[i] = (struct record_range){ .inode = inode, .start = i ? 200 + 2 * i : 0, .end = i ? 200 + 2 * i : 100, .owner = 1, .type = NOS_LOCK_READ };
    struct record_range snapshot[RECORD_LOCK_LIMIT]; memcpy(snapshot, record_locks, sizeof(snapshot));
    ok = lock_set(1, a, NOS_LOCK_WRITE, 50, 1) == -NOS_ENOLCK && record_lock_count == RECORD_LOCK_LIMIT && !memcmp(snapshot, record_locks, sizeof(snapshot));
    record_lock_owner_exit(1); ok &= !record_lock_count;
    check(ok, "record_locks_enolck_split_rollback_owner_exit");
    int gc_owner = open_file(4, "/tmp/gc", NOS_O_CREAT | NOS_O_RDWR), gc_waiter = open_file(5, "/tmp/gc", NOS_O_RDWR);
    ok = gc_owner >= 3 && gc_waiter >= 3 && !lock_set(4, gc_owner, NOS_LOCK_WRITE, 0, 0);
    ok &= change_lock(5, 5, gc_waiter, NOS_LOCK_SET_WAIT, NOS_LOCK_WRITE, 0, 0, 0, 1, NULL) == HOST_PENDING;
    /* Intentionally omit the owner's final PEER_EXIT notice. */
    group_state(4, 0); batch_error = -NOS_EFAULT;
    unsigned queries_before = batch_queries; service_complete();
    check(existing_table(4) && record_lock_count == 1 && record_locks[0].owner == 4 &&
          waits() == 1 && !calls[5].completed && batch_queries == queries_before + 1 && !scalar_queries,
          "record_locks_batch_query_failure_retains_entire_state");
    batch_error = 0; service_complete();
    ok &= !existing_table(4) && calls[5].completed && !calls[5].result && record_lock_count == 1 && record_locks[0].owner == 5;
    group_state(5, 0); service_complete(); ok &= !existing_table(5) && !record_lock_count && !waits();
    check(ok, "record_locks_group_gc_lost_exit_wakes_waiter");
    int protected_fd = open_file(6, "/tmp/gc", NOS_O_RDWR);
    ok = protected_fd >= 3 && !lock_set(6, protected_fd, NOS_LOCK_WRITE, 0, 0);
    group_state(6, -NOS_EACCES); service_complete(); ok &= existing_table(6) && record_lock_count == 1;
    group_state(6, 1); service_complete(); ok &= existing_table(6) && record_lock_count == 1;
    group_state(6, 0); int fresh_fd = open_file(18, "/tmp/newgeneration", NOS_O_CREAT | NOS_O_RDWR);
    ok &= fresh_fd >= 3 && !existing_table(6) && existing_table(18) && !lock_set(18, fresh_fd, NOS_LOCK_WRITE, 0, 0);
    service_complete(); ok &= existing_table(18) && record_lock_count == 1 && record_locks[0].owner == 18;
    group_state(18, 0); service_complete();
    check(ok, "record_locks_group_gc_query_error_and_full_generation");
    int parent_fd = open_file(7, "/tmp/gc", NOS_O_RDWR);
    ok = parent_fd >= 3 && !lock_set(7, parent_fd, NOS_LOCK_WRITE, 0, 0);
    unsigned parent_description = table_for(7)->fd[parent_fd] - 1;
    group_state(7, 2); service_complete();
    ok &= existing_table(7) && !record_lock_count;
    /* The first fork is delivered while another one still pins this parent. */
    ok &= !inherit_files(7, 8); service_complete();
    ok &= existing_table(7) && existing_table(8) && existing_table(8)->fd[parent_fd] == parent_description + 1;
    ok &= !inherit_files(7, 9); group_state(7, 0); service_complete();
    ok &= !existing_table(7) && existing_table(8) && existing_table(9) &&
        existing_table(9)->fd[parent_fd] == parent_description + 1 && opened[parent_description].refs == 2;
    ok &= !lock_set(9, parent_fd, NOS_LOCK_WRITE, 0, 0);
    ok &= change_lock(8, 8, parent_fd, NOS_LOCK_SET_WAIT, NOS_LOCK_WRITE, 0, 0, 0, 1, NULL) == HOST_PENDING;
    calls[8].active = 0; group_state(8, 0); service_complete();
    ok &= !existing_table(8) && !waits() && opened[parent_description].refs == 1;
    ok &= !lock_set(9, parent_fd, NOS_LOCK_UNLOCK, 0, 0) && !record_lock_count;
    group_state(9, 0); service_complete();
    check(ok, "record_locks_group_gc_fork_pins_and_dead_waiter");
    struct nos_message pipe_request = { .op = NOS_VFS_PIPE };
    ok = !invoke(10, 10, &pipe_request, 1);
    int pipe_pair[2]; memcpy(pipe_pair, pipe_request.data, sizeof(pipe_pair));
    group_state(10, NOS_GROUP_FORK_REFERENCES);
    ok &= !inherit_files(10, 22); service_complete();
    ok &= existing_table(10) && !close_file(22, 22, pipe_pair[1], 1);
    struct nos_message read_request = { .op = NOS_VFS_READ, .arg = pipe_pair[0], .len = 1 };
    ok &= invoke(22, 22, &read_request, 1) == HOST_PENDING;
    /* Model the next RECV acknowledging the final delivered fork, followed
     * by a notice. The real kernel's guarantee of that event is guest-tested
     * separately; this checks the VFS order and deferred EOF wakeup only. */
    group_state(10, 0); service_complete();
    ok &= !existing_table(10) && calls[22 % NOS_TASKS].completed &&
        !calls[22 % NOS_TASKS].result && !calls[22 % NOS_TASKS].reply.len;
    group_state(22, 0); service_complete();
    check(ok, "record_locks_group_gc_last_fork_ack_pipe_eof");
    unsigned tables = 0;
    for (unsigned i = 0; i < FD_TABLE_COUNT; i++) tables += fds[i].pid >= 0;
    for (unsigned owner = 100; tables < NOS_TASKS - 1; owner++, tables++) ok &= table_for(owner) != NULL;
    int capacity_parent = open_file(200, "/tmp/capacity", NOS_O_CREAT | NOS_O_RDWR);
    ok = capacity_parent >= 3 && !lock_set(200, capacity_parent, NOS_LOCK_WRITE, 0, 0);
    unsigned inherited_description = table_for(200)->fd[capacity_parent] - 1;
    group_state(200, NOS_GROUP_FORK_REFERENCES);
    ok &= !inherit_files(200, 201); service_complete();
    ok &= existing_table(200) && existing_table(201) && !record_lock_count;
    ok &= !inherit_files(200, 202); group_state(200, 0); service_complete();
    ok &= !existing_table(200) && existing_table(201) && existing_table(202) &&
        existing_table(201)->fd[capacity_parent] == inherited_description + 1 &&
        existing_table(202)->fd[capacity_parent] == inherited_description + 1 && opened[inherited_description].refs == 2;
    check(ok, "record_locks_group_gc_full_live_table_fork_inheritance");
    for (unsigned owner = 1000; ; owner++) { if (!table_for(owner)) break; }
    struct fd_table before_parent = *existing_table(201);
    unsigned before_references = opened[inherited_description].refs;
    ok = inherit_files(201, 3000) == -NOS_ENOSPC && !memcmp(&before_parent, existing_table(201), sizeof(before_parent)) &&
        opened[inherited_description].refs == before_references && !existing_table(3000);
    check(ok, "record_locks_fork_table_exhaustion_preserves_parent");
    printf("RECORD LOCK HOST %s (%u failures; mocked transport only)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
