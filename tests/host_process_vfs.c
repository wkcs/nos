/* Copyright (C) 2026 NOS contributors. Multi-batch VFS GC with mock IPC. */
#include "abi.h"
static intptr_t process_query_hook(uintptr_t address, uintptr_t count, uintptr_t flags);
#define NOS_HOST_GROUP_QUERY_HOOK process_query_hook
#define main nos_record_lock_host_main
#include "host_record_locks.c"
#undef main

#define PROCESS_BATCHES ((FD_TABLE_COUNT + NOS_GROUP_QUERY_MAX - 1u) / NOS_GROUP_QUERY_MAX)
static unsigned hook_calls, failure_at, mutation_at, query_sizes[PROCESS_BATCHES];
static uintptr_t mutation_owner;
static int hook_invalid;
static unsigned process_owner(unsigned slot) { return 1000u + slot * NOS_TASKS; }
static intptr_t process_query_hook(uintptr_t address, uintptr_t count, uintptr_t flags)
{
    struct nos_group_state *query = (void *)address;
    hook_calls++;
    if (flags != NOS_GROUP_QUERY_BATCH || !count || count > NOS_GROUP_QUERY_MAX || !query) hook_invalid = 1;
    if (hook_calls <= PROCESS_BATCHES) query_sizes[hook_calls - 1] = (unsigned)count;
    if (mutation_at && hook_calls == mutation_at) group_state(mutation_owner, 0);
    return failure_at && hook_calls == failure_at ? -NOS_EACCES : 0;
}
static void reset_queries(void)
{
    hook_calls = failure_at = mutation_at = hook_invalid = 0;
    mutation_owner = 0; memset(query_sizes, 0, sizeof(query_sizes));
}
static void process_reset(void)
{
    memset(nodes, 0, sizeof(nodes)); memset(security, 0, sizeof(security));
    memset(opened, 0, sizeof(opened)); memset(fds, 0, sizeof(fds)); memset(pipes, 0, sizeof(pipes));
    memset(calls, 0, sizeof(calls)); memset(host_groups, 0, sizeof(host_groups));
    memset(pending_io, 0, sizeof(pending_io)); memset(record_locks, 0, sizeof(record_locks));
    memset(record_waits, 0, sizeof(record_waits)); memset(&subject, 0, sizeof(subject));
    memset(&host_credentials, 0, sizeof(host_credentials));
    disk_mounted = fat_mounted = record_lock_count = 0;
    host_nofile_soft = host_nofile_hard = NOS_FD_LIMIT;
    batch_error = 0; fail_reply = 0; batch_queries = scalar_queries = 0;
    reset_queries(); initialize();
}
static unsigned process_tables(void)
{
    unsigned count = 0;
    for (unsigned i = 0; i < FD_TABLE_COUNT; i++) count += fds[i].pid >= 0;
    return count;
}
static struct {
    struct fd_table tables[FD_TABLE_COUNT];
    struct description descriptions[OPEN_COUNT];
    struct vnode nodes[NODE_COUNT];
    struct pending_io pending[NOS_TASKS];
    struct record_range locks[RECORD_LOCK_LIMIT];
    struct record_wait records[NOS_TASKS];
    struct pipe_buffer pipes[PIPE_COUNT];
    struct host_call calls[NOS_TASKS];
    unsigned lock_count;
} saved;
static void save_state(void)
{
#define SAVE(member, input) memcpy(saved.member, input, sizeof(input))
    SAVE(tables, fds); SAVE(descriptions, opened); SAVE(nodes, nodes);
    SAVE(pending, pending_io); SAVE(locks, record_locks); SAVE(records, record_waits);
    SAVE(pipes, pipes); SAVE(calls, calls); saved.lock_count = record_lock_count;
#undef SAVE
}
static int state_unchanged(void)
{
    return !memcmp(saved.tables, fds, sizeof(fds)) && !memcmp(saved.descriptions, opened, sizeof(opened)) &&
        !memcmp(saved.nodes, nodes, sizeof(nodes)) && !memcmp(saved.pending, pending_io, sizeof(pending_io)) &&
        !memcmp(saved.locks, record_locks, sizeof(record_locks)) && !memcmp(saved.records, record_waits, sizeof(record_waits)) &&
        !memcmp(saved.pipes, pipes, sizeof(pipes)) && !memcmp(saved.calls, calls, sizeof(calls)) &&
        saved.lock_count == record_lock_count;
}
static unsigned holder_description, waiting_description, pipe_description;
static int holder_fd, waiting_fd;
static int process_fixture(void)
{
    process_reset();
    holder_fd = open_file_as(process_owner(0), 1, "/tmp/process-gc-lock", NOS_O_CREAT | NOS_O_RDWR);
    waiting_fd = open_file_as(process_owner(1), 2, "/tmp/process-gc-lock", NOS_O_RDWR);
    if (holder_fd < 3 || waiting_fd < 3) return 0;
    holder_description = table_for(process_owner(0))->fd[holder_fd] - 1;
    waiting_description = table_for(process_owner(1))->fd[waiting_fd] - 1;
    if (change_lock(process_owner(0), 1, holder_fd, NOS_LOCK_SET, NOS_LOCK_WRITE, 0, 1, 0, 1, NULL)) return 0;
    if (change_lock(process_owner(1), 2, waiting_fd, NOS_LOCK_SET_WAIT, NOS_LOCK_WRITE, 0, 1, 0, 1, NULL) != HOST_PENDING) return 0;
    struct nos_message pipe = { .op = NOS_VFS_PIPE };
    if (invoke(process_owner(2), 79, &pipe, 1)) return 0;
    int pair[2]; memcpy(pair, pipe.data, sizeof(pair));
    pipe_description = table_for(process_owner(2))->fd[pair[0]] - 1;
    struct nos_message read = { .op = NOS_VFS_READ, .arg = pair[0], .len = 1 };
    if (invoke(process_owner(2), 79, &read, 1) != HOST_PENDING) return 0;
    for (unsigned i = 0; i < FD_TABLE_COUNT; i++) {
        group_state(process_owner(i), NOS_GROUP_LIVE_TASKS);
        if (!table_for(process_owner(i))) return 0;
    }
    reset_queries();
    return process_tables() == FD_TABLE_COUNT && waits() == 1 && pipe_wait_count() == 1;
}
static void mark_retired(void)
{
    group_state(process_owner(0), NOS_GROUP_FORK_REFERENCES);
    static const unsigned retired[] = {2, 63, 64, 128, FD_TABLE_COUNT - 1};
    for (unsigned i = 0; i < sizeof(retired) / sizeof(retired[0]); i++) group_state(process_owner(retired[i]), 0);
}
static int committed_cleanup(void)
{
    return !hook_invalid && hook_calls == PROCESS_BATCHES && process_tables() == FD_TABLE_COUNT - 5 &&
        existing_table(process_owner(0)) && existing_table(process_owner(1)) &&
        !existing_table(process_owner(2)) && !existing_table(process_owner(63)) &&
        !existing_table(process_owner(64)) && !existing_table(process_owner(128)) &&
        !existing_table(process_owner(FD_TABLE_COUNT - 1)) && !record_lock_count &&
        waits() == 1 && !pipe_wait_count() && opened[waiting_description].refs == 2 &&
        opened[holder_description].refs == 1 && !opened[pipe_description].refs &&
        nodes[0].refs == FD_TABLE_COUNT - 5;
}
static void batch_failure_checks(void)
{
    static const unsigned failures_at[] = {1, 2, PROCESS_BATCHES};
    int ok = FD_TABLE_COUNT == 203 && OPEN_COUNT == 1280 && PROCESS_BATCHES == 4;
    for (unsigned i = 0; i < sizeof(failures_at) / sizeof(failures_at[0]); i++) {
        if (!process_fixture()) { ok = 0; break; }
        mark_retired(); save_state(); failure_at = failures_at[i];
        unsigned before = batch_queries;
        collect_process_owners();
        ok &= state_unchanged() && hook_calls == failure_at && batch_queries == before + failure_at && !hook_invalid;
        reset_queries(); collect_process_owners();
        ok &= committed_cleanup();
    }
    check(ok, "vfs_process_gc_first_middle_last_failures_preserve_state");
    ok = !hook_invalid && hook_calls == PROCESS_BATCHES;
    for (unsigned i = 0; i < PROCESS_BATCHES; i++)
        ok &= query_sizes[i] == (i + 1 == PROCESS_BATCHES ? FD_TABLE_COUNT - i * NOS_GROUP_QUERY_MAX : NOS_GROUP_QUERY_MAX);
    check(ok, "vfs_process_gc_batches_cover_203_full_generation_owners");
}
static void invalid_last_state_check(void)
{
    int ok = process_fixture();
    if (ok) {
        mark_retired(); group_state(process_owner(FD_TABLE_COUNT - 1), -NOS_EACCES); save_state();
        collect_process_owners();
        ok &= hook_calls == PROCESS_BATCHES && state_unchanged();
        group_state(process_owner(FD_TABLE_COUNT - 1), 8); reset_queries();
        collect_process_owners();
        ok &= hook_calls == PROCESS_BATCHES && state_unchanged();
        group_state(process_owner(FD_TABLE_COUNT - 1), 0); reset_queries();
        collect_process_owners(); ok &= committed_cleanup();
    }
    check(ok, "vfs_process_gc_invalid_last_state_preserves_state");
}
static void late_liveness_and_fork_check(void)
{
    int ok = process_fixture();
    if (ok) {
        group_state(process_owner(0), NOS_GROUP_FORK_REFERENCES);
        mutation_at = 2; mutation_owner = process_owner(FD_TABLE_COUNT - 1);
        collect_process_owners();
        ok &= hook_calls == PROCESS_BATCHES && process_tables() == FD_TABLE_COUNT - 1 &&
            !existing_table(mutation_owner) && existing_table(process_owner(0)) && existing_table(process_owner(1)) &&
            !record_lock_count && waits() == 1 && pipe_wait_count() == 1;
        unsigned child = process_owner(FD_TABLE_COUNT + 1);
        group_state(child, NOS_GROUP_LIVE_TASKS);
        ok &= !inherit_files(process_owner(0), child) && opened[holder_description].refs == 2;
        group_state(process_owner(0), 0); reset_queries(); collect_process_owners();
        ok &= !existing_table(process_owner(0)) && existing_table(process_owner(1)) && existing_table(child) &&
            existing_table(child)->fd[holder_fd] == holder_description + 1 && opened[holder_description].refs == 1;
        record_lock_complete();
        ok &= !waits() && calls[2].completed && !calls[2].result && opened[waiting_description].refs == 1;
    }
    check(ok, "vfs_process_gc_late_liveness_generation_and_fork_pins");
}
static void reference_bound_check(void)
{
    process_reset();
    unsigned owner = process_owner(0);
    int fd = open_file_as(owner, 1, "/tmp/process-ref-bound", NOS_O_CREAT | NOS_O_RDWR);
    int ok = fd >= 3;
    if (ok) {
        unsigned description = table_for(owner)->fd[fd] - 1;
        for (unsigned i = 0; i < FDS; i++) ok &= duplicate(owner, fd, (int)i) == (int)i;
        ok &= opened[description].refs == FDS && !lock_set(owner, fd, NOS_LOCK_WRITE, 0, 1);
        for (unsigned i = 1; i < FD_TABLE_COUNT; i++) ok &= !inherit_files(owner, process_owner(i));
        ok &= opened[description].refs == FD_TABLE_COUNT * FDS;
        for (unsigned i = 0; i < NOS_TASKS; i++)
            ok &= change_lock(process_owner(1), i, fd, NOS_LOCK_SET_WAIT, NOS_LOCK_WRITE, 0, 1, 0, 1, NULL) == HOST_PENDING;
        ok &= waits() == NOS_TASKS && opened[description].refs == FD_TABLE_COUNT * FDS + NOS_TASKS;
        for (unsigned i = 0; i < FD_TABLE_COUNT; i++) group_state(process_owner(i), 0);
        reset_queries(); collect_process_owners();
        ok &= !process_tables() && !record_lock_count && !waits() && !opened[description].refs && !nodes[0].refs;
    }
    check(ok, "vfs_process_gc_6576_ofd_references_reclaim_exactly");
}
int main(void)
{
    batch_failure_checks(); invalid_last_state_check(); late_liveness_and_fork_check(); reference_bound_check();
    for (unsigned i = 0; i < sizeof(host_mappings) / sizeof(host_mappings[0]); i++) free(host_mappings[i]);
    for (unsigned i = 0; i < sizeof(host_shared) / sizeof(host_shared[0]); i++) free(host_shared[i].address);
    printf("VFS PROCESS HOST %s (%u failures; mocked transport only)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
