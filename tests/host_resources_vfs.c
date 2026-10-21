/* Copyright (C) 2026 NOS contributors. Actual VFS resource admission, mock IPC. */
#define main nos_record_lock_host_main
#include "host_record_locks.c"
#undef main

static void resources_reset(void)
{
    memset(nodes, 0, sizeof(nodes)); memset(security, 0, sizeof(security));
    memset(opened, 0, sizeof(opened)); memset(fds, 0, sizeof(fds)); memset(pipes, 0, sizeof(pipes));
    memset(calls, 0, sizeof(calls)); memset(host_groups, 0, sizeof(host_groups));
    memset(pending_io, 0, sizeof(pending_io)); memset(record_locks, 0, sizeof(record_locks));
    memset(&host_credentials, 0, sizeof(host_credentials)); memset(&subject, 0, sizeof(subject));
    disk_mounted = fat_mounted = host_fat_fixture = host_fat_list_secret = 0;
    fat_path[0] = mount_path[0] = 0; record_lock_count = 0;
    subject_error = clock_error = limits_error = 0; limits_mismatch = limits_cancel = 0;
    host_nofile_soft = host_nofile_hard = NOS_FD_LIMIT;
    initialize();
}
static intptr_t resources_dup(int fd, intptr_t target, unsigned minimum, unsigned cloexec)
{
    struct nos_message message = { .op = NOS_VFS_DUP, .arg = fd, .value = target, .len = minimum };
    message.data[0] = (char)cloexec; return invoke(1, 1, &message, 1);
}
static intptr_t resources_write(int fd, char byte)
{
    struct nos_message message = { .op = NOS_VFS_WRITE, .arg = fd, .len = 1, .data = { byte } };
    return invoke(1, 1, &message, 1);
}
static intptr_t resources_read_at(int fd, char *byte)
{ return bulk_pread(fd, 0, byte, 1, sizeof(uint64_t)); }
static intptr_t resources_pipe(int pair[2], unsigned flags)
{
    struct nos_message message = { .op = NOS_VFS_PIPE, .arg = flags };
    intptr_t result = invoke(1, 1, &message, 1);
    if (!result) memcpy(pair, message.data, sizeof(int) * 2);
    return result;
}
static unsigned resources_descriptions(void)
{ unsigned count = 0; for (unsigned i = 0; i < OPEN_COUNT; i++) count += !!opened[i].refs; return count; }
static unsigned resources_pipe_ends(void)
{ unsigned count = 0; for (unsigned i = 0; i < PIPE_COUNT; i++) count += pipes[i].readers + pipes[i].writers; return count; }
int main(void)
{
    resources_reset();
    int fd = open_file(1, "/tmp/bounds", NOS_O_CREAT | NOS_O_RDWR);
    int high = (int)resources_dup(fd, 12, 0, 0); char byte = 0;
    int ok = fd == 3 && high == 12 && resources_write(high, 'A') == 1;
    host_nofile_soft = 4;
    ok &= open_file(1, "/tmp/denied", NOS_O_CREAT | NOS_O_RDWR) == -NOS_EMFILE && lookup("/tmp/denied") == -NOS_ENOENT;
    ok &= resources_dup(fd, -2, 0, 0) == -NOS_EMFILE;
    ok &= resources_dup(fd, -1, 4, 0) == -NOS_EINVAL;
    ok &= resources_read_at(high, &byte) == 1 && byte == 'A';
    struct nos_message stat = { .op = NOS_VFS_STAT64, .arg = high };
    ok &= !invoke(1, 1, &stat, 1) && resources_dup(high, high, 0, 0) == high;
    ok &= !close_file(1, 1, fd, 1) && resources_dup(high, -2, 0, 0) == 3;
    ok &= !close_file(1, 1, 3, 1) && open_file(1, "/tmp/hole", NOS_O_CREAT | NOS_O_RDWR) == 3;
    check(ok, "vfs_resources_new_fd_boundary_existing_high_fd_survives");

    resources_reset(); fd = open_file(1, "/tmp/source", NOS_O_CREAT | NOS_O_RDWR);
    int target = (int)resources_dup(fd, 12, 0, 1);
    ok = fd == 3 && target == 12 && !lock_set(1, target, NOS_LOCK_WRITE, 0, 1) && record_lock_count == 1;
    unsigned before_ref = opened[table_for(1)->fd[target] - 1].refs;
    unsigned before_flags = table_for(1)->fd_flags;
    host_nofile_soft = 4;
    ok &= resources_dup(fd, target, 0, 0) == -NOS_EBADF && record_lock_count == 1 &&
        opened[table_for(1)->fd[target] - 1].refs == before_ref && table_for(1)->fd_flags == before_flags;
    ok &= resources_dup(target, target, 0, 0) == target && record_lock_count == 1;
    ok &= resources_dup(target, target, 0, 1) == -NOS_EINVAL && record_lock_count == 1;
    check(ok, "vfs_resources_failed_dup2_preserves_target_flags_and_locks");

    resources_reset(); fd = open_file(1, "/tmp/zero", NOS_O_CREAT | NOS_O_RDWR);
    host_nofile_soft = 0;
    ok = fd == 3 && resources_dup(fd, -2, 0, 0) == -NOS_EMFILE &&
        resources_dup(fd, -1, 0, 0) == -NOS_EINVAL && resources_dup(fd, fd, 0, 0) == fd &&
        open_file(1, "/tmp/not_created", NOS_O_CREAT | NOS_O_RDWR) == -NOS_EMFILE &&
        lookup("/tmp/not_created") == -NOS_ENOENT && resources_write(fd, 'Z') == 1;
    host_nofile_soft = 8;
    ok &= resources_dup(fd, -1, 7, 1) == 7 && ((table_for(1)->fd_flags >> 7) & 1u) &&
        resources_dup(fd, -1, 8, 0) == -NOS_EINVAL;
    check(ok, "vfs_resources_zero_limit_dup_and_fcntl_minimum_errors");

    resources_reset(); table_for(1); host_nofile_soft = 4;
    unsigned before_open = resources_descriptions(), before_ends = resources_pipe_ends();
    int pair[2] = { 91, 92 };
    ok = resources_pipe(pair, NOS_O_CLOEXEC) == -NOS_EMFILE && pair[0] == 91 && pair[1] == 92 &&
        resources_descriptions() == before_open && resources_pipe_ends() == before_ends && !table_for(1)->fd[3];
    host_nofile_soft = 5;
    ok &= !resources_pipe(pair, NOS_O_CLOEXEC | NOS_O_NONBLOCK) && pair[0] == 3 && pair[1] == 4 &&
        resources_descriptions() == before_open + 2 && resources_pipe_ends() == before_ends + 2 &&
        ((table_for(1)->fd_flags >> 3) & 3u) == 3;
    host_nofile_soft = 0;
    ok &= resources_write(pair[1], 'P') == 1;
    struct nos_message read = { .op = NOS_VFS_READ, .arg = pair[0], .len = 1 };
    ok &= invoke(1, 1, &read, 1) == 1 && read.data[0] == 'P' &&
        !close_file(1, 1, pair[0], 1) && !close_file(1, 1, pair[1], 1) &&
        resources_descriptions() == before_open && resources_pipe_ends() == before_ends;
    check(ok, "vfs_resources_pipe_two_fd_admission_atomic_and_recoverable");

    resources_reset(); fd = open_file(1, "/tmp/live", NOS_O_CREAT | NOS_O_RDWR);
    host_nofile_soft = 4;
    struct nos_message spoof = { .op = NOS_VFS_OPEN, .arg = NOS_O_CREAT | NOS_O_RDWR,
        .value = NOS_FD_LIMIT, .len = sizeof("/tmp/spoof") };
    strcpy(spoof.data, "/tmp/spoof");
    ok = invoke(1, 1, &spoof, 1) == -NOS_EMFILE;
    host_nofile_soft = 5;
    ok &= open_file(1, "/tmp/live2", NOS_O_CREAT | NOS_O_RDWR) == 4;
    host_nofile_soft = 4;
    ok &= resources_dup(fd, -2, 0, 0) == -NOS_EMFILE;
    check(ok, "vfs_resources_live_trusted_limits_ignore_client_spoof");

    resources_reset(); fd = open_file(1, "/tmp/kept", NOS_O_CREAT | NOS_O_RDWR);
    ok = resources_write(fd, 'K') == 1;
    static struct description saved_opened[OPEN_COUNT];
    static struct vnode saved_nodes[NODE_COUNT];
    static char saved_names[NODE_COUNT][NOS_PATH_MAX];
    static struct fd_table saved_fds[FD_TABLE_COUNT];
    memcpy(saved_opened, opened, sizeof(opened)); memcpy(saved_nodes, nodes, sizeof(nodes)); memcpy(saved_fds, fds, sizeof(fds));
    for (unsigned i = 0; i < NODE_COUNT; i++) memcpy(saved_names[i], nodes[i].name, NOS_PATH_MAX);
    for (unsigned failure = 0; failure < 7; failure++) {
        limits_error = failure == 0 ? -NOS_ENOTSUP : failure == 1 ? -NOS_EPERM : 0;
        limits_mismatch = failure >= 2 && failure <= 4 ? failure - 1 : 0;
        host_nofile_soft = failure == 5 ? NOS_FD_LIMIT + 1u : NOS_FD_LIMIT;
        host_nofile_hard = failure == 6 ? NOS_FD_LIMIT + 1u : NOS_FD_LIMIT;
        intptr_t expected = failure == 0 ? -NOS_ENOTSUP : -NOS_EPERM;
        ok &= open_file(1, "/tmp/fail_closed", NOS_O_CREAT | NOS_O_TRUNC | NOS_O_RDWR) == expected &&
            !memcmp(saved_opened, opened, sizeof(opened)) && !memcmp(saved_nodes, nodes, sizeof(nodes)) && !memcmp(saved_fds, fds, sizeof(fds));
        for (unsigned i = 0; i < NODE_COUNT; i++) ok &= !memcmp(saved_names[i], nodes[i].name, NOS_PATH_MAX);
    }
    limits_error = -NOS_ENOTSUP; limits_mismatch = 0;
    ok &= resources_read_at(fd, &byte) == 1 && byte == 'K';
    check(ok, "vfs_resources_missing_invalid_or_mismatched_limit_fails_closed");
    return failures ? 1 : 0;
}
