/* Copyright (C) 2026 NOS contributors. Actual DAC helpers, explicit mock IPC. */
#define main nos_record_lock_host_main
#include "host_record_locks.c"
#undef main
static void reset_credentials_test(void)
{
    memset(nodes, 0, sizeof(nodes)); memset(security, 0, sizeof(security));
    memset(opened, 0, sizeof(opened)); memset(fds, 0, sizeof(fds)); memset(pipes, 0, sizeof(pipes));
    memset(calls, 0, sizeof(calls)); memset(host_groups, 0, sizeof(host_groups));
    memset(pending_io, 0, sizeof(pending_io)); memset(record_locks, 0, sizeof(record_locks));
    memset(&host_credentials, 0, sizeof(host_credentials)); memset(&subject, 0, sizeof(subject));
    disk_mounted = fat_mounted = host_fat_fixture = host_fat_list_secret = 0; fat_path[0] = mount_path[0] = 0;
    record_lock_count = 0; subject_error = clock_error = 0; host_seconds = 1700000000;
    initialize();
}
static void identity(uint32_t uid, uint32_t gid)
{
    memset(&host_credentials, 0, sizeof(host_credentials));
    host_credentials.ruid = host_credentials.euid = host_credentials.suid = uid;
    host_credentials.rgid = host_credentials.egid = host_credentials.sgid = gid;
}
static intptr_t path_request(unsigned op, const char *path, intptr_t arg, intptr_t value)
{
    struct nos_message message = { .op = op, .arg = arg, .value = value, .len = strlen(path) + 1 };
    if (message.len <= NOS_MSG_DATA) strcpy(message.data, path);
    else { message.bulk_in = (uintptr_t)path; message.bulk_in_len = message.len; message.len = 0; }
    return invoke(1, 1, &message, 1);
}
static int create_mode(const char *path, unsigned mode)
{ return (int)path_request(NOS_VFS_OPENAT_MODE, path, (mode << 20) | NOS_O_CREAT | NOS_O_RDWR, -100); }
static intptr_t chmod_path(const char *path, unsigned mode)
{ return path_request(NOS_VFS_CHMOD, path, -100, mode); }
static intptr_t chown_path(const char *path, uint32_t uid, uint32_t gid)
{
    uint32_t ids[2] = { uid, gid };
    struct nos_message message = { .op = NOS_VFS_CHOWN, .arg = -100, .len = sizeof(ids),
        .bulk_in = (uintptr_t)path, .bulk_in_len = strlen(path) + 1 };
    memcpy(message.data, ids, sizeof(ids)); return invoke(1, 1, &message, 1);
}
static intptr_t access_path(const char *path, unsigned mode, unsigned flags)
{ return path_request(NOS_VFS_ACCESS, path, -100, mode | (flags << 8)); }
static intptr_t two_paths(unsigned op, const char *a, const char *b)
{
    char bytes[PATH_DATA_MAX]; size_t first = strlen(a) + 1, second = strlen(b) + 1;
    memcpy(bytes, a, first); memcpy(bytes + first, b, second);
    struct nos_message message = { .op = op, .arg = -100, .value = -100,
        .bulk_in = (uintptr_t)bytes, .bulk_in_len = first + second };
    return invoke(1, 1, &message, 1);
}
static intptr_t unlink_path(const char *path)
{ return path_request(NOS_VFS_UNLINK, path, 0, 0); }
static int mkdir_mode(const char *path, unsigned mode)
{ return (int)path_request(NOS_VFS_MKDIRAT, path, -100, mode); }
static int fd_stat(int fd, struct nos_stat64 *st)
{
    struct nos_message message = { .op = NOS_VFS_STAT64, .arg = fd };
    int rc = (int)invoke(1, 1, &message, 1); if (!rc) memcpy(st, message.data, sizeof(*st)); return rc;
}
static intptr_t fd_write(int fd, const char *text)
{
    struct nos_message message = { .op = NOS_VFS_WRITE, .arg = fd, .len = strlen(text) };
    memcpy(message.data, text, message.len); return invoke(1, 1, &message, 1);
}
static intptr_t resolve_flags(const char *path, char output[NOS_PATH_MAX], uintptr_t flags)
{
    struct nos_message message = { .op = NOS_VFS_RESOLVE_PATH, .arg = -100, .value = (intptr_t)flags,
        .bulk_in = (uintptr_t)path, .bulk_in_len = strlen(path) + 1,
        .bulk_out = (uintptr_t)output, .bulk_out_len = NOS_PATH_MAX };
    return invoke(1, 1, &message, 1);
}
int main(void)
{
    reset_credentials_test();
    int fd = create_mode("/tmp/owned", 0640); struct nos_stat64 st;
    int ok = fd >= 3 && !chown_path("/tmp/owned", 100, 200) && !fd_stat(fd, &st) && st.uid == 100 && st.gid == 200 && st.mtime_sec == host_seconds;
    identity(100, 300); ok &= access_path("/tmp/owned", 6, 0) == 0;
    identity(101, 200); ok &= access_path("/tmp/owned", 4, 0) == 0 && access_path("/tmp/owned", 2, 0) == -NOS_EACCES;
    identity(101, 300); ok &= access_path("/tmp/owned", 4, 0) == -NOS_EACCES;
    host_credentials.groups[0] = 200; host_credentials.ngroups = 1;
    ok &= !access_path("/tmp/owned", 4, 0);
    check(ok, "vfs_credentials_owner_group_other_supplementary");

    host_credentials.ruid = 100; host_credentials.rgid = 300; host_credentials.ngroups = 0;
    ok = !access_path("/tmp/owned", 4, 0) && access_path("/tmp/owned", 4, 0x200) == -NOS_EACCES;
    ok &= open_file(1, "/tmp/owned", 0) == -NOS_EACCES;
    identity(0, 0); ok &= access_path("/tmp/owned", 1, 0) == -NOS_EACCES && !chmod_path("/tmp/owned", 0641) && !access_path("/tmp/owned", 1, 0);
    ok &= access_path("/tmp/owned", 0, 0x1000) == -NOS_EINVAL;
    check(ok, "vfs_credentials_real_effective_access_and_root_execute");

    reset_credentials_test();
    char deep[NOS_PATH_MAX] = "/tmp", after[NOS_PATH_MAX], hard[NOS_PATH_MAX];
    char symbolic[NOS_PATH_MAX], resolved[NOS_PATH_MAX];
    ok = 1;
    for (unsigned component = 0; component < 4; component++) {
        size_t end = strlen(deep); deep[end++] = '/';
        memset(deep + end, 'A' + component, 50); deep[end + 50] = 0;
        ok &= !mkdir_mode(deep, 0700);
    }
    size_t end = strlen(deep);
    memcpy(after, deep, end); strcpy(after + end, "/after");
    memcpy(hard, deep, end); strcpy(hard + end, "/hard");
    memcpy(symbolic, deep, end); strcpy(symbolic + end, "/symbolic");
    fd = create_mode(after, 0600);
    ok &= fd >= 3 && strlen(after) > 200 && strlen(after) + strlen(hard) + 2 > 400;
    if (fd >= 0) close_file(1, 1, fd, 1);
    ok &= !two_paths(NOS_VFS_LINK, after, hard) && !two_paths(NOS_VFS_SYMLINK, after, symbolic);
    ok &= resolve_flags(symbolic, resolved, 0) == (intptr_t)(strlen(after) + 1) && !strcmp(resolved, after);
    memset(resolved, 0x5a, sizeof(resolved));
    ok &= resolve_flags(symbolic, resolved, NOS_RESOLVE_EXEC) == -NOS_EACCES;
    for (unsigned i = 0; i < sizeof(resolved); i++) ok &= resolved[i] == 0x5a;
    ok &= resolve_flags(symbolic, resolved, 2) == -NOS_EINVAL;
    for (unsigned i = 0; i < sizeof(resolved); i++) ok &= resolved[i] == 0x5a;
    ok &= !chmod_path(hard, 0700);
    ok &= resolve_flags(symbolic, resolved, NOS_RESOLVE_EXEC) == (intptr_t)(strlen(after) + 1) && !strcmp(resolved, after);
    identity(100, 100);
    ok &= resolve_flags(symbolic, resolved, 0) == -NOS_EACCES;
    ok &= resolve_flags(symbolic, resolved, NOS_RESOLVE_EXEC) == -NOS_EACCES;
    identity(0, 0); ok &= !chmod_path(hard, 0600);
    ok &= resolve_flags(after, resolved, NOS_RESOLVE_EXEC) == -NOS_EACCES;
    ok &= resolve_flags(deep, resolved, 0) == (intptr_t)(strlen(deep) + 1) && !strcmp(resolved, deep);
    memset(resolved, 0x5a, sizeof(resolved));
    ok &= resolve_flags(deep, resolved, NOS_RESOLVE_EXEC) == -NOS_EACCES;
    for (unsigned i = 0; i < sizeof(resolved); i++) ok &= resolved[i] == 0x5a;
    check(ok, "vfs_credentials_long_path_exec_symlink_hardlink_modes");

    reset_credentials_test();
    ok = !mkdir_mode("/tmp/deny", 0700) && !mkdir_mode("/tmp/allow", 0777);
    int outside = create_mode("/tmp/escape", 0644); if (outside >= 0) close_file(1, 1, outside, 1);
    identity(100, 100);
    ok &= access_path("/tmp/deny/../escape", 4, 0) == -NOS_EACCES;
    ok &= access_path("/tmp/deny/./x", 0, 0) == -NOS_EACCES;
    ok &= access_path("/tmp/deny/", 0, 0) == -NOS_EACCES;
    ok &= access_path("/tmp/deny", 0, 0) == 0;
    identity(0, 0); ok &= !two_paths(NOS_VFS_SYMLINK, "/tmp/deny/../escape", "/tmp/link");
    identity(100, 100); ok &= access_path("/tmp/link", 4, 0) == -NOS_EACCES;
    check(ok, "vfs_credentials_component_search_dotdot_symlink_trailing_slash");

    reset_credentials_test();
    ok = !mkdir_mode("/tmp/parent", 0777) && !mkdir_mode("/tmp/parent/current", 0777);
    int dirfd = open_file(1, "/tmp/parent/current", NOS_O_DIRECTORY);
    outside = create_mode("/tmp/parent/current/data", 0644); if (outside >= 0) close_file(1, 1, outside, 1);
    ok &= dirfd >= 3 && !path_request(NOS_VFS_CHDIR, "/tmp/parent/current", 0, 0) && !chmod_path("/tmp/parent", 0000);
    identity(100, 100);
    int relative = (int)path_request(NOS_VFS_OPENAT, "data", 0, dirfd);
    ok &= relative >= 3; if (relative >= 0) close_file(1, 1, relative, 1);
    relative = (int)path_request(NOS_VFS_OPENAT, "data", 0, -100);
    ok &= relative >= 3; if (relative >= 0) close_file(1, 1, relative, 1);
    ok &= path_request(NOS_VFS_OPENAT, "../current/data", 0, dirfd) == -NOS_EACCES;
    check(ok, "vfs_credentials_relative_dirfd_cwd_does_not_recheck_ancestors");

    reset_credentials_test();
    fd = create_mode("/tmp/kept", 0600); ok = fd >= 3;
    identity(100, 100);
    ok &= fd_write(fd, "kept") == 4 && open_file(1, "/tmp/kept", NOS_O_RDWR) == -NOS_EACCES;
    struct nos_message change = { .op = NOS_VFS_FCHMOD, .arg = fd, .value = 0666 };
    ok &= invoke(1, 1, &change, 1) == -NOS_EPERM;
    identity(0, 0); ok &= !unlink_path("/tmp/kept"); identity(100, 100);
    ok &= fd_write(fd, "open") == 4 && !fd_stat(fd, &st) && st.uid == 0 && st.nlink == 0;
    check(ok, "vfs_credentials_open_ofd_survives_drop_chmod_unlink");

    reset_credentials_test();
    fd = create_mode("/tmp/old", 0600); ok = fd >= 3 && !two_paths(NOS_VFS_LINK, "/tmp/old", "/tmp/alias");
    ok &= !chown_path("/tmp/alias", 100, 200) && !fd_stat(fd, &st) && st.uid == 100 && st.gid == 200 && st.nlink == 2;
    identity(100, 200); ok &= !chmod_path("/tmp/old", 0640);
    ok &= !unlink_path("/tmp/old") && !fd_stat(fd, &st) && st.uid == 100 && st.gid == 200 && (st.mode & 0777) == 0640 && st.nlink == 1;
    ok &= !unlink_path("/tmp/alias") && !fd_stat(fd, &st) && st.nlink == 0;
    close_file(1, 1, fd, 1); identity(300, 400);
    fd = create_mode("/tmp/new", 0600); ok &= fd >= 3 && !fd_stat(fd, &st) && st.uid == 300 && st.gid == 400;
    check(ok, "vfs_credentials_hardlinks_metadata_unlinked_inode_reuse");

    reset_credentials_test();
    fd = create_mode("/tmp/victim", 0644); if (fd >= 0) close_file(1, 1, fd, 1);
    ok = !chown_path("/tmp/victim", 100, 100);
    identity(200, 200); fd = create_mode("/tmp/source", 0644); if (fd >= 0) close_file(1, 1, fd, 1);
    ok &= unlink_path("/tmp/victim") == -NOS_EPERM;
    ok &= two_paths(NOS_VFS_RENAMEAT, "/tmp/source", "/tmp/victim") == -NOS_EPERM;
    ok &= lookup("/tmp/source") >= 0 && lookup("/tmp/victim") >= 0;
    identity(100, 100); ok &= !unlink_path("/tmp/victim");
    identity(200, 200); ok &= !two_paths(NOS_VFS_RENAMEAT, "/tmp/source", "/tmp/moved");
    check(ok, "vfs_credentials_sticky_unlink_rename_target_rollback");

    reset_credentials_test();
    ok = !mkdir_mode("/tmp/group", 0777) && !chown_path("/tmp/group", 100, 200) && !chmod_path("/tmp/group", 02777);
    identity(100, 300);
    fd = create_mode("/tmp/group/file", 0666); ok &= fd >= 3 && !fd_stat(fd, &st) && st.uid == 100 && st.gid == 200 && (st.mode & 0777) == 0644;
    ok &= !mkdir_mode("/tmp/group/child", 0777);
    int child = lookup("/tmp/group/child"); ok &= child >= 0 && (nodes[child].mode & 02000) && security[child].gid == 200;
    ok &= chown_path("/tmp/group/file", UINT32_MAX, 201) == -NOS_EPERM;
    ok &= !fd_stat(fd, &st) && st.gid == 200;
    host_credentials.groups[0] = 201; host_credentials.ngroups = 1;
    ok &= !chown_path("/tmp/group/file", UINT32_MAX, 201) && !fd_stat(fd, &st) && st.gid == 201;
    ok &= chown_path("/tmp/group/file", 101, UINT32_MAX) == -NOS_EPERM;
    ok &= chmod_path("/tmp/group/file", 04755) == -NOS_ENOTSUP;
    check(ok, "vfs_credentials_setgid_umask_chown_sentinel_group_rules");

    reset_credentials_test();
    fd = create_mode("/tmp/times", 0600); ok = fd >= 3 && !fd_stat(fd, &st);
    int64_t creation = st.ctime_sec; host_seconds++;
    ok &= fd_write(fd, "x") == 1 && !fd_stat(fd, &st) && st.mtime_sec == host_seconds && st.ctime_sec == host_seconds && st.atime_sec == creation;
    int node = lookup("/tmp/times"); struct inode_security previous = security[node]; unsigned oldmode = nodes[node].mode;
    clock_error = -NOS_EIO;
    ok &= chmod_path("/tmp/times", 0777) == -NOS_EIO && nodes[node].mode == oldmode && !memcmp(&previous, &security[node], sizeof(previous));
    ok &= fd_write(fd, "no") == -NOS_EIO && nodes[node].size == 1;
    clock_error = 0; host_seconds++;
    ok &= !chmod_path("/tmp/times", 0644) && security[node].ctime == host_seconds;
    check(ok, "vfs_credentials_real_seconds_clock_failure_rollback");

    reset_credentials_test();
    fd = open_file(1, "/proc/sys/kernel/ngroups_max", 0);
    struct nos_message read = { .op = NOS_VFS_READ, .arg = fd, .len = 16 };
    ok = fd >= 3 && invoke(1, 1, &read, 1) == 3 && !memcmp(read.data, "32\n", 3);
    ok &= open_file(1, "/proc/sys/kernel/ngroups_max", NOS_O_WRONLY) == -NOS_EACCES;
    subject_error = -NOS_EPERM;
    ok &= open_file(1, "/tmp/unauthorized", NOS_O_CREAT | NOS_O_RDWR) == -NOS_EPERM && lookup("/tmp/unauthorized") == -NOS_ENOENT;
    subject_error = 0;
    struct nos_message forged = { .sender = 7, .process = 1, .token = UINT64_MAX, .op = NOS_VFS_OPEN, .arg = NOS_O_CREAT, .len = 11 };
    strcpy(forged.data, "/tmp/forge"); service(&forged);
    ok &= forged.value == -NOS_EPERM && lookup("/tmp/forge") == -NOS_ENOENT;
    check(ok, "vfs_credentials_proc_limit_and_missing_subject_fail_closed");
    reset_credentials_test(); identity(123, 456);
    struct nos_message pipe_message = { .op = NOS_VFS_PIPE };
    ok = !invoke(1, 1, &pipe_message, 1);
    int pair[2]; memcpy(pair, pipe_message.data, sizeof(pair));
    ok &= !fd_stat(pair[0], &st) && st.uid == 123 && st.gid == 456 && st.ctime_sec == host_seconds;
    check(ok, "vfs_credentials_pipe_creation_owner_independent_of_root_vnode");

    reset_credentials_test(); host_fat_fixture = 1;
    ok = !fat_mount("/tmp/mount", 0);
    fd = create_mode("/tmp/mount/SECRET", 0600);
    ok &= fd >= 3 && !fd_stat(fd, &st) && (st.mode & 0777) == 0600 && st.uid == 0;
    if (fd >= 0) close_file(1, 1, fd, 1);
    for (unsigned index = 0; index < NODE_COUNT; index++) if (nodes[index].persistent) clear_node(index);
    fat_mounted = 0; fat_path[0] = 0; host_fat_list_secret = 1;
    ok &= !fat_mount("/tmp/mount", 0);
    fd = open_file(1, "/tmp/mount/SECRET", 0);
    ok &= fd >= 3 && !fd_stat(fd, &st) && (st.mode & 0777) == 0666 && st.uid == 0;
    identity(100, 100);
    ok &= access_path("/tmp/mount/SECRET", 4, 0) == -NOS_EACCES;
    ok &= open_file(1, "/tmp/mount/SECRET", 0) == -NOS_EACCES;
    ok &= create_mode("/tmp/mount/OTHER", 0600) == -NOS_EACCES;
    ok &= path_request(NOS_VFS_STATAT, "/tmp/mount", -100, 0) == -NOS_EACCES;
    struct nos_message inherited_read = { .op = NOS_VFS_READ, .arg = fd, .len = 1 };
    ok &= invoke(1, 1, &inherited_read, 1) == 1 && inherited_read.data[0] == 'S';
    check(ok, "vfs_credentials_persistent_reimport_root_mount_blocks_mode_leak");
    return failures ? 1 : 0;
}
