/* Copyright (C) 2026 NOS contributors. Actual VFS paths; transport is mocked. */
#define main nos_record_lock_host_main
#include "host_record_locks.c"
#undef main

static void paths_reset(void)
{
    for (unsigned i = 0; i < NODE_COUNT; i++) if (file_backing[i].handle) clear_node(i);
    memset(nodes, 0, sizeof(nodes)); memset(security, 0, sizeof(security));
    memset(opened, 0, sizeof(opened)); memset(fds, 0, sizeof(fds)); memset(pipes, 0, sizeof(pipes));
    memset(calls, 0, sizeof(calls)); memset(host_groups, 0, sizeof(host_groups));
    memset(pending_io, 0, sizeof(pending_io)); memset(record_locks, 0, sizeof(record_locks));
    memset(&host_credentials, 0, sizeof(host_credentials)); memset(&subject, 0, sizeof(subject));
    disk_mounted = fat_mounted = host_fat_fixture = host_fat_list_secret = 0;
    fat_path[0] = mount_path[0] = 0; record_lock_count = 0;
    subject_error = clock_error = limits_error = fetch_error = store_error = fetch_second_error = 0;
    limits_mismatch = limits_cancel = fetch_cancel = store_cancel = fetch_second_cancel = cancel_after_store = 0;
    host_nofile_soft = host_nofile_hard = NOS_FD_LIMIT;
    initialize();
}
static intptr_t paths_request(unsigned op, const char *path, intptr_t arg, intptr_t value)
{
    struct nos_message message = { .op = op, .arg = arg, .value = value,
        .bulk_in = (uintptr_t)path, .bulk_in_len = strlen(path) + 1 };
    return invoke(1, 1, &message, 1);
}
static int paths_mkdir(const char *path)
{ return (int)paths_request(NOS_VFS_MKDIRAT, path, -100, 0777); }
static int paths_create(const char *path)
{ return (int)paths_request(NOS_VFS_OPENAT_MODE, path, (0600u << 20) | NOS_O_CREAT | NOS_O_RDWR, -100); }
static intptr_t paths_pair(unsigned op, const char *first, const char *second)
{
    uint32_t sizes[2] = { (uint32_t)strlen(first) + 1, (uint32_t)strlen(second) + 1 };
    struct nos_message message = { .op = op, .arg = -100, .value = -100, .len = sizeof(sizes),
        .bulk_in = (uintptr_t)first, .bulk_in_len = sizes[0] };
    memcpy(message.data, sizes, sizeof(sizes));
    return invoke_inputs(1, 1, &message, 1, (uintptr_t)second, sizes[1], 2);
}
static intptr_t paths_cwd(char *output, size_t capacity)
{
    struct nos_message message = { .op = NOS_VFS_GETCWD, .bulk_out = (uintptr_t)output, .bulk_out_len = capacity };
    return invoke(1, 1, &message, 1);
}
static int paths_stat_fd(int fd, struct nos_stat64 *stat)
{
    struct nos_message message = { .op = NOS_VFS_STAT64, .arg = fd };
    int result = (int)invoke(1, 1, &message, 1); if (!result) memcpy(stat, message.data, sizeof(*stat)); return result;
}
static int paths_fixture(char deep[NOS_PATH_MAX], char file[NOS_PATH_MAX], char other[NOS_PATH_MAX])
{
    strcpy(deep, "/tmp");
    for (unsigned i = 0; i < 15; i++) {
        size_t at = strlen(deep); deep[at++] = '/';
        memset(deep + at, 'A' + i, NOS_NAME_MAX); deep[at + NOS_NAME_MAX] = 0;
        if (paths_mkdir(deep)) return 0;
    }
    size_t at = strlen(deep); memcpy(file, deep, at); file[at++] = '/';
    memset(file + at, 'f', NOS_PATH_MAX - at - 1); file[NOS_PATH_MAX - 1] = 0;
    memcpy(other, file, NOS_PATH_MAX); other[at] = 'g';
    return strlen(file) == NOS_PATH_MAX - 1;
}
static unsigned paths_nodes(void)
{ unsigned count = 0; for (unsigned i = 0; i < NODE_COUNT; i++) count += !!nodes[i].used; return count; }
static unsigned shared_objects(void)
{ unsigned count = 0; for (unsigned i = 0; i < sizeof(host_shared) / sizeof(host_shared[0]); i++) count += !!host_shared[i].handle; return count; }

int main(void)
{
    char deep[NOS_PATH_MAX], file[NOS_PATH_MAX], other[NOS_PATH_MAX], output[NOS_PATH_MAX + 1];
    paths_reset();
    int ok = NOS_PATH_MAX == 4096 && NOS_NAME_MAX == 255 && paths_fixture(deep, file, other);
    int fd = paths_create(file);
    ok &= fd >= 3 && lookup(file) >= 0;
    if (fd >= 0) ok &= !close_file(1, 1, fd, 1);
    char component[NOS_NAME_MAX + 7]; strcpy(component, "/tmp/");
    memset(component + 5, 'x', NOS_NAME_MAX + 1); component[5 + NOS_NAME_MAX + 1] = 0;
    unsigned before = paths_nodes();
    ok &= paths_mkdir(component) == -NOS_ENAMETOOLONG && paths_nodes() == before;
    char malformed[NOS_PATH_MAX]; memset(malformed, 'x', sizeof(malformed)); malformed[0] = '/';
    struct nos_message malformed_path = { .op = NOS_VFS_OPEN, .arg = NOS_O_CREAT,
        .bulk_in = (uintptr_t)malformed, .bulk_in_len = sizeof(malformed) };
    ok &= invoke(1, 1, &malformed_path, 1) == -NOS_EINVAL && paths_nodes() == before;
    check(ok, "vfs_path4096_exact_boundary_and_name255");

    int inode = lookup(file);
    ok = inode >= 0 && !paths_pair(NOS_VFS_RENAME, file, other) && lookup(file) == -NOS_ENOENT && lookup(other) == inode;
    ok &= !paths_pair(NOS_VFS_LINK, other, file) && lookup(file) == inode && node_links((unsigned)inode) == 2;
    check(ok, "vfs_path4096_two_read_spans_8192_atomic_namespace");

    ok = !paths_request(NOS_VFS_UNLINK, file, 0, 0);
    unsigned before_nodes = paths_nodes();
    struct inode_security before_security[NODE_COUNT]; memcpy(before_security, security, sizeof(security));
    static struct vnode before_nodes_state[NODE_COUNT];
    static char before_names[NODE_COUNT][NOS_PATH_MAX];
    memcpy(before_nodes_state, nodes, sizeof(nodes));
    for (unsigned i = 0; i < NODE_COUNT; i++) memcpy(before_names[i], nodes[i].name, NOS_PATH_MAX);
    fetch_second_error = -NOS_EFAULT;
    ok &= paths_pair(NOS_VFS_RENAME, other, file) == -NOS_EFAULT && lookup(other) == inode && lookup(file) == -NOS_ENOENT;
    fetch_second_error = 0; fetch_second_cancel = 1;
    ok &= paths_pair(NOS_VFS_RENAME, other, file) == -NOS_EPERM && lookup(other) == inode && lookup(file) == -NOS_ENOENT;
    fetch_second_cancel = 0;
    ok &= paths_nodes() == before_nodes && !memcmp(before_security, security, sizeof(security)) &&
        !memcmp(before_nodes_state, nodes, sizeof(nodes));
    for (unsigned i = 0; i < NODE_COUNT; i++) ok &= !memcmp(before_names[i], nodes[i].name, NOS_PATH_MAX);
    check(ok, "vfs_path4096_second_fetch_error_cancel_preserves_namespace");

    unsigned old_objects = shared_objects();
    ok = !paths_pair(NOS_VFS_SYMLINK, other, file);
    int symbolic = lookup(file);
    ok &= symbolic >= 0 && nodes[symbolic].size == NOS_PATH_MAX - 1 && shared_objects() == old_objects + 1;
    memset(output, 0xa7, sizeof(output));
    struct nos_message readlink = { .op = NOS_VFS_READLINK, .arg = -100,
        .bulk_in = (uintptr_t)file, .bulk_in_len = NOS_PATH_MAX,
        .bulk_out = (uintptr_t)output, .bulk_out_len = 17 };
    ok &= invoke(1, 1, &readlink, 1) == 17 && !memcmp(output, other, 17) && (unsigned char)output[17] == 0xa7;
    struct nos_message resolve = { .op = NOS_VFS_RESOLVE_PATH, .arg = -100,
        .bulk_in = (uintptr_t)file, .bulk_in_len = NOS_PATH_MAX,
        .bulk_out = (uintptr_t)output, .bulk_out_len = NOS_PATH_MAX };
    ok &= invoke(1, 1, &resolve, 1) == NOS_PATH_MAX && !strcmp(output, other);
    ok &= !paths_request(NOS_VFS_UNLINK, file, 0, 0) && shared_objects() == old_objects;
    check(ok, "vfs_path4096_long_symlink_backing_readlink_truncation_cleanup");

    fd = open_file(1, deep, NOS_O_DIRECTORY);
    struct nos_stat64 before_stat, after_stat;
    ok = fd >= 3 && !paths_stat_fd(fd, &before_stat) && !paths_request(NOS_VFS_CHDIR, deep, 0, 0);
    char old_top[NOS_NAME_MAX + 6], new_top[NOS_NAME_MAX + 6];
    memcpy(old_top, deep, sizeof(old_top) - 1); old_top[sizeof(old_top) - 1] = 0;
    memcpy(new_top, old_top, sizeof(old_top)); new_top[5] = 'Z';
    ok &= !paths_pair(NOS_VFS_RENAME, old_top, new_top);
    deep[5] = 'Z';
    ok &= paths_cwd(output, sizeof(output) - 1) == (intptr_t)strlen(deep) + 1 && !strcmp(output, deep);
    ok &= !paths_stat_fd(fd, &after_stat) && before_stat.ino == after_stat.ino;
    memset(output, 0xa7, sizeof(output));
    ok &= paths_cwd(output, 3) == -NOS_ERANGE && (unsigned char)output[0] == 0xa7;
    if (fd >= 0) ok &= !close_file(1, 1, fd, 1);
    check(ok, "vfs_path4096_pinned_cwd_tracks_ancestor_rename");

    paths_reset();
    ok = !paths_mkdir("/tmp/old");
    fd = open_file(1, "/tmp/old", NOS_O_DIRECTORY);
    ok &= fd >= 3 && !paths_stat_fd(fd, &before_stat) && !paths_request(NOS_VFS_CHDIR, "/tmp/old", 0, 0);
    ok &= !paths_request(NOS_VFS_RMDIR, "/tmp/old", 0, 0) && paths_cwd(output, sizeof(output) - 1) == -NOS_ENOENT;
    ok &= !paths_mkdir("/tmp/old") && !paths_stat_fd(fd, &after_stat) && after_stat.ino == before_stat.ino && !after_stat.nlink;
    int replacement = open_file(1, "/tmp/old", NOS_O_DIRECTORY);
    ok &= replacement >= 3 && !paths_stat_fd(replacement, &after_stat) && after_stat.ino != before_stat.ino;
    ok &= paths_request(NOS_VFS_OPENAT, "new", NOS_O_CREAT | NOS_O_RDWR, fd) == -NOS_ENOENT && lookup("/tmp/old/new") == -NOS_ENOENT;
    struct nos_message fchdir = { .op = NOS_VFS_FCHDIR, .arg = fd };
    ok &= !invoke(1, 1, &fchdir, 1) && paths_cwd(output, sizeof(output) - 1) == -NOS_ENOENT;
    ok &= !paths_request(NOS_VFS_CHDIR, "/tmp", 0, 0);
    if (fd >= 0) ok &= !close_file(1, 1, fd, 1);
    if (replacement >= 0) ok &= !close_file(1, 1, replacement, 1);
    check(ok, "vfs_path4096_unlinked_cwd_not_retargeted_by_replacement");

    paths_reset();
    ok = !paths_mkdir("/tmp/store");
    int persistent = lookup("/tmp/store");
    if (persistent >= 0) nodes[persistent].persistent = 1;
    char disk_name[65]; strcpy(disk_name, "/tmp/store/");
    memset(disk_name + 11, 'd', 52); disk_name[63] = 0;
    fd = paths_create(disk_name); ok &= fd >= 3;
    if (fd >= 0) ok &= !close_file(1, 1, fd, 1);
    disk_name[63] = 'd'; disk_name[64] = 0;
    ok &= paths_create(disk_name) == -NOS_ENAMETOOLONG;
    check(ok, "vfs_path4096_persistent_path64_boundary_unchanged");

    ok = path_scratch_used == 0 && path_scratch_highwater > 2u * NOS_PATH_MAX &&
        path_scratch_highwater <= VFS_PATH_SCRATCH_BYTES;
    for (unsigned iteration = 0; iteration < 16; iteration++) {
        ok &= !canonical_path("/tmp", 5, output) && !strcmp(output, "/tmp") && !path_scratch_used;
        ok &= invoke(1, 1, &malformed_path, 1) == -NOS_EINVAL && !path_scratch_used;
    }
    for (unsigned i = 0; i < NODE_COUNT; i++)
        ok &= nodes[i].name == path_storage + (size_t)i * NOS_PATH_MAX;
    check(ok, "vfs_path4096_bounded_scratch_reclaimed");
    printf("METRIC path_scratch_highwater=%zu path_scratch_capacity=%u namespace_bytes=%u\n",
           path_scratch_highwater, VFS_PATH_SCRATCH_BYTES,
           VFS_NAME_BYTES + VFS_PATH_SCRATCH_BYTES);
    return failures ? 1 : 0;
}
