/* Copyright (C) 2026 NOS contributors. Actual VFS capacity with mock IPC. */
#define main nos_record_lock_host_main
#include "host_record_locks.c"
#undef main

static void capacity_reset(void)
{
    memset(nodes, 0, sizeof(nodes)); memset(security, 0, sizeof(security));
    memset(opened, 0, sizeof(opened)); memset(fds, 0, sizeof(fds)); memset(pipes, 0, sizeof(pipes));
    memset(calls, 0, sizeof(calls)); memset(host_groups, 0, sizeof(host_groups));
    memset(pending_io, 0, sizeof(pending_io)); memset(record_locks, 0, sizeof(record_locks));
    memset(record_waits, 0, sizeof(record_waits)); memset(&subject, 0, sizeof(subject));
    memset(&host_credentials, 0, sizeof(host_credentials));
    disk_mounted = fat_mounted = record_lock_count = 0;
    host_nofile_soft = host_nofile_hard = NOS_FD_LIMIT;
    initialize();
}
static unsigned capacity_tables(void)
{
    unsigned count = 0;
    for (unsigned i = 0; i < FD_TABLE_COUNT; i++) count += fds[i].pid >= 0;
    return count;
}
static int capacity_pipe(unsigned owner, unsigned sender, int pair[2])
{
    struct nos_message request = { .op = NOS_VFS_PIPE };
    intptr_t result = invoke(owner, sender, &request, 1);
    if (!result) memcpy(pair, request.data, sizeof(int) * 2);
    return (int)result;
}
static intptr_t capacity_read(unsigned owner, unsigned sender, int fd)
{
    struct nos_message request = { .op = NOS_VFS_READ, .arg = fd, .len = 1 };
    return invoke(owner, sender, &request, 1);
}
static void capacity_pipe_waiters(void)
{
    capacity_reset();
    int pair[2] = { -1, -1 };
    int ok = NOS_TASKS == 80 && NOS_PROCESSES == 12 && OPEN_COUNT == 384 && FD_TABLE_COUNT == 63;
    ok &= !capacity_pipe(1000, 1, pair);
    if (pair[0] < 0) { check(0, "vfs_capacity_64_high_slot_readers_share_process_fds"); return; }
    unsigned reader = table_for(1000)->fd[pair[0]] - 1;
    for (unsigned slot = 16; slot < 80; slot++)
        ok &= capacity_read(1000, slot, pair[0]) == HOST_PENDING;
    ok &= capacity_tables() == 1 && pipe_wait_count() == 64 && opened[reader].refs == 65;
    struct nos_message write = { .op = NOS_VFS_WRITE, .arg = pair[1], .len = 64 };
    for (unsigned i = 0; i < 64; i++) write.data[i] = (char)(i + 1);
    ok &= invoke(1000, 1, &write, 1) == 64 && !pipe_wait_count() && opened[reader].refs == 1;
    for (unsigned slot = 16; slot < 80; slot++)
        ok &= calls[slot].completed && calls[slot].result == 1 &&
            calls[slot].reply.len == 1 && calls[slot].reply.data[0] == (char)(slot - 15);
    check(ok, "vfs_capacity_64_high_slot_readers_share_process_fds");

    ok = capacity_read(1000, 79, pair[0]) == HOST_PENDING;
    uint64_t old_token = calls[79].token;
    /* A later generation reuses task slot 79 before its old cancellation is
     * delivered. The old watermark releases only the obsolete OFD pin. */
    ok &= capacity_read(1000, 79 + NOS_TASKS, pair[0]) == HOST_PENDING && pipe_wait_count() == 2;
    uint64_t new_token = calls[79].token;
    pipe_cancel(79, old_token, 1);
    ok &= pipe_wait_count() == 1 && opened[reader].refs == 2 && calls[79].active;
    struct nos_message byte = { .op = NOS_VFS_WRITE, .arg = pair[1], .len = 1, .data = { 'N' } };
    ok &= invoke(1000, 1, &byte, 1) == 1 && !pipe_wait_count() && opened[reader].refs == 1 &&
        calls[79].completed && calls[79].token == new_token && calls[79].result == 1 && calls[79].reply.data[0] == 'N';
    check(ok, "vfs_capacity_high_slot_generation_cancel_preserves_new_request");
}
static void capacity_record_waiters(void)
{
    capacity_reset();
    int holder = open_file_as(1000, 1, "/tmp/capacity-lock", NOS_O_CREAT | NOS_O_RDWR);
    int waiting = open_file_as(1080, 2, "/tmp/capacity-lock", NOS_O_RDWR);
    if (holder < 3 || waiting < 3) { check(0, "vfs_capacity_64_record_waiters_keep_task_capacity"); return; }
    int ok = holder >= 3 && waiting >= 3 &&
        !change_lock(1000, 1, holder, NOS_LOCK_SET, NOS_LOCK_WRITE, 0, 1, 0, 1, NULL);
    for (unsigned slot = 16; slot < 80; slot++)
        ok &= change_lock(1080, slot, waiting, NOS_LOCK_SET_WAIT, NOS_LOCK_WRITE, 0, 1, 0, 1, NULL) == HOST_PENDING;
    unsigned description = table_for(1080)->fd[waiting] - 1;
    ok &= waits() == 64 && capacity_tables() == 2 && opened[description].refs == 65;
    ok &= !change_lock(1000, 1, holder, NOS_LOCK_SET, NOS_LOCK_UNLOCK, 0, 1, 0, 1, NULL);
    ok &= !waits() && opened[description].refs == 1 && record_lock_count == 1;
    for (unsigned slot = 16; slot < 80; slot++) ok &= calls[slot].completed && !calls[slot].result;
    check(ok, "vfs_capacity_64_record_waiters_keep_task_capacity");
}
static void capacity_owner_gc(void)
{
    capacity_reset();
    int pair[2] = { -1, -1 };
    int ok = !capacity_pipe(1000, 1, pair);
    if (pair[0] < 0) { check(0, "vfs_capacity_full_generation_gc_pending_fork_pins"); return; }
    unsigned reader = table_for(1000)->fd[pair[0]] - 1;
    for (unsigned i = 0; i < FD_TABLE_COUNT; i++) {
        unsigned owner = 1000 + i * NOS_TASKS;
        group_state(owner, NOS_GROUP_LIVE_TASKS);
        ok &= table_for(owner) != NULL;
    }
    ok &= capacity_tables() == FD_TABLE_COUNT && !table_for(9000);
    ok &= capacity_read(1000, 79, pair[0]) == HOST_PENDING && opened[reader].refs == 2;
    group_state(1000, NOS_GROUP_FORK_REFERENCES);
    unsigned batches = batch_queries;
    batch_error = -NOS_EACCES; service_complete(); batch_error = 0;
    ok &= batch_queries == batches + 1 && pipe_wait_count() == 1 && opened[reader].refs == 2 && existing_table(1000);
    service_complete();
    ok &= batch_queries == batches + 2 && !scalar_queries && !pipe_wait_count() &&
        existing_table(1000) && existing_table(1080) && opened[reader].refs == 1;
    /* The fork-referenced old generation retains its FD table even while a
     * new process generation with the same task-slot residue remains live. */
    struct fd_table before = *existing_table(1000);
    ok &= inherit_files(1000, 9000) == -NOS_ENOSPC &&
        !memcmp(&before, existing_table(1000), sizeof(before)) && opened[reader].refs == 1;
    group_state(1000 + (FD_TABLE_COUNT - 1) * NOS_TASKS, 0); service_complete();
    ok &= !inherit_files(1000, 9000) && opened[reader].refs == 2;
    group_state(1000, 0); service_complete();
    ok &= !existing_table(1000) && existing_table(1080) && existing_table(9000) &&
        existing_table(9000)->fd[pair[0]] == reader + 1 && opened[reader].refs == 1;
    check(ok, "vfs_capacity_full_generation_gc_pending_fork_pins");
}
static void capacity_record_graph(void)
{
    capacity_reset();
    /* Model old generations retained during unavailable liveness queries as
     * well as live owners. Their full IDs deliberately share a task residue. */
    record_lock_count = RECORD_LOCK_LIMIT;
    for (unsigned i = 0; i < RECORD_LOCK_LIMIT; i++) {
        record_locks[i] = (struct record_range){ .inode = i + 1, .owner = 1000 + i * NOS_TASKS,
            .start = 0, .end = 0, .type = NOS_LOCK_WRITE };
        if (i + 1 < RECORD_LOCK_LIMIT) {
            pending_io[i].used = RECORD_WAIT_KIND;
            record_waits[i].range = (struct record_range){ .inode = i + 2, .owner = 1000 + i * NOS_TASKS,
                .start = 0, .end = 0, .type = NOS_LOCK_WRITE };
        }
    }
    struct record_range request = { .inode = 1, .owner = 90000, .start = 0, .end = 0, .type = NOS_LOCK_WRITE };
    int ok = RECORD_LOCK_LIMIT == 64 && !record_deadlock(&request);
    unsigned last = RECORD_LOCK_LIMIT - 1;
    pending_io[last].used = RECORD_WAIT_KIND;
    record_waits[last].range = (struct record_range){ .inode = 1, .owner = 1000 + last * NOS_TASKS,
        .start = 0, .end = 0, .type = NOS_LOCK_WRITE };
    /* A cycle elsewhere in the graph must terminate without falsely accusing
     * this request. Moving the request into that cycle must detect deadlock. */
    ok &= !record_deadlock(&request);
    request.owner = 1000 + last * NOS_TASKS;
    ok &= record_deadlock(&request) == -NOS_EDEADLK;
    check(ok, "vfs_capacity_64_owner_graph_chain_and_cycles");
}
int main(void)
{
    capacity_pipe_waiters(); capacity_record_waiters(); capacity_owner_gc(); capacity_record_graph();
    for (unsigned i = 0; i < sizeof(host_mappings) / sizeof(host_mappings[0]); i++) free(host_mappings[i]);
    for (unsigned i = 0; i < sizeof(host_shared) / sizeof(host_shared[0]); i++) free(host_shared[i].address);
    printf("VFS CAPACITY HOST %s (%u failures; mocked transport only)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
