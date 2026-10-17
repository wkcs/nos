/* Copyright (C) 2026 NOS contributors */
#include "abi.h"
#include "user.h"
#include "thread.h"
#include "../drivers/devices.h"
static intptr_t request(unsigned op, intptr_t arg, intptr_t value)
{ struct nos_message m = { .op = op, .arg = arg, .value = value }; return nos_rpc(&m); }
static int remove_dir(const char *path)
{ struct nos_message m = { .op = NOS_VFS_RMDIR }; m.len = u_strlen(path) + 1; u_memcpy(m.data, path, m.len); return (int)nos_rpc(&m); }
static int move(const char *old, const char *next)
{
    struct nos_message m = { .op = NOS_VFS_RENAME }; size_t a = u_strlen(old) + 1, b = u_strlen(next) + 1;
    if (a + b > NOS_MSG_DATA) return -NOS_ENAMETOOLONG;
    u_memcpy(m.data, old, a); u_memcpy(m.data + a, next, b); m.len = a + b; return (int)nos_rpc(&m);
}
void nos_vfs_regression(void)
{
    char bytes[8]; int fd = nos_open("/tmp/p0", NOS_O_CREAT | NOS_O_RDWR), held[NOS_FD_LIMIT], count = 0;
    int ok = fd >= 3 && nos_write(fd, "SECRET", 6) == 6;
    while ((unsigned)count < NOS_FD_LIMIT) { int extra = nos_open("/tmp/p0", NOS_O_RDONLY); if (extra < 0) break; held[count++] = extra; }
    ok &= nos_open("/tmp/p0", NOS_O_RDWR | NOS_O_TRUNC) == -NOS_EMFILE;
    ok &= nos_open("/tmp/absent", NOS_O_CREAT | NOS_O_RDWR) == -NOS_EMFILE;
    struct nos_stat st; ok &= !nos_stat_fd(fd, &st) && st.size == 6;
    for (int i = 0; i < count; i++) nos_close(held[i]);
    ok &= nos_open("/tmp/absent", NOS_O_RDONLY) == -NOS_ENOENT;
    nos_check(ok, "vfs_emfile_transaction");
    nos_close(fd); fd = nos_open("/tmp/p0", NOS_O_RDWR | NOS_O_TRUNC);
    u_memset(bytes, 0xff, sizeof(bytes));
    ok = fd >= 3 && nos_seek(fd, 5, 0) == 5 && nos_write(fd, "X", 1) == 1 && !nos_seek(fd, 0, 0) && nos_read(fd, bytes, 6) == 6;
    for (unsigned i = 0; i < 5; i++) ok &= !bytes[i];
    ok &= bytes[5] == 'X';
    nos_check(ok, "vfs_sparse_zero");
    ok = !nos_unlink("/tmp/p0") && nos_open("/tmp/p0", 0) == -NOS_ENOENT && nos_seek(fd, 5, 0) == 5 && nos_read(fd, bytes, 1) == 1 && bytes[0] == 'X';
    nos_check(ok, "vfs_unlink_open"); nos_close(fd);
    fd = nos_open("/tmp//./file..txt", NOS_O_CREAT | NOS_O_RDWR);
    ok = fd >= 3 && nos_write(fd, "AB", 2) == 2;
    int second = nos_open("/tmp/file..txt", 0); ok &= second >= 3; if (second >= 3) nos_close(second);
    nos_check(ok, "vfs_path_normalization");
    int duplicate = (int)request(NOS_VFS_DUP, fd, -1);
    ok = duplicate >= 3 && !nos_seek(fd, 0, 0) && nos_read(duplicate, bytes, 1) == 1 && bytes[0] == 'A' && nos_read(fd, bytes, 1) == 1 && bytes[0] == 'B';
    nos_check(ok, "vfs_dup_shared_offset"); nos_close(duplicate); nos_close(fd); nos_unlink("/tmp/file..txt");
    struct nos_message m = { .op = NOS_VFS_PIPE }; int pair[2] = { -1, -1 };
    ok = !nos_rpc(&m); if (ok) u_memcpy(pair, m.data, sizeof(pair));
    ok &= nos_write(pair[1], "IPC", 3) == 3 && nos_read(pair[0], bytes, 3) == 3 && bytes[0] == 'I' && bytes[2] == 'C';
    ok &= !nos_close(pair[1]) && !nos_read(pair[0], bytes, 1); nos_close(pair[0]);
    nos_check(ok, "vfs_pipe");
    ok = !nos_mkdir("/tmp/rename"); fd = nos_open("/tmp/rename/before", NOS_O_CREAT | NOS_O_RDWR);
    ok &= fd >= 3 && !move("/tmp/rename/before", "/tmp/rename/after") && nos_open("/tmp/rename/before", 0) == -NOS_ENOENT;
    int dir = nos_open("/tmp/rename", NOS_O_RDONLY); m = (struct nos_message){ .op = NOS_VFS_READDIR, .arg = dir };
    ok &= dir >= 3 && nos_rpc(&m) == 1 && u_streq(m.data, "after");
    ok &= nos_write(fd, "R", 1) == 1 && !nos_close(fd) && !nos_close(dir) && remove_dir("/tmp/rename") == -NOS_ENOTEMPTY;
    ok &= !nos_unlink("/tmp/rename/after") && !remove_dir("/tmp/rename");
    nos_check(ok, "vfs_rename_readdir");
    ok = 1;
    for (unsigned i = 0; i < 1000; i++) {
        fd = nos_open("/tmp/cycle", NOS_O_CREAT | NOS_O_RDWR);
        if (fd < 3 || nos_write(fd, "L", 1) != 1 || nos_close(fd) || nos_unlink("/tmp/cycle")) { ok = 0; break; }
    }
    nos_check(ok, "vfs_resource_cycles");
    nos_check(nos_syscall(NOS_SYS_DEVICE, 0, 0, 0) == -NOS_EACCES, "device_capability");
}
static int archive_request(unsigned op, intptr_t dirfd, const char *path, intptr_t flags, struct nos_message *reply)
{
    *reply = (struct nos_message){ .op = op, .arg = dirfd, .value = flags };
    reply->len = u_strlen(path) + 1; u_memcpy(reply->data, path, reply->len); return (int)nos_rpc(reply);
}
static void metadata_regression(void)
{
    struct nos_stat64 st, linked; char data[16];
    int old_mask = nos_umask(0027);
    int fd = nos_openat_mode(NOS_AT_FDCWD, "/tmp/meta", NOS_O_CREAT | NOS_O_RDWR, 0666);
    int ok = fd >= 0 && !nos_stat64_fd(fd, &st) && (st.mode & 0777) == 0640;
    ok &= nos_write(fd, "abcdef", 6) == 6 && nos_seek(fd, 2, 0) == 2;
    ok &= !nos_ftruncate(fd, 3) && !nos_ftruncate(fd, 8) && nos_seek(fd, 0, 1) == 2;
    u_memset(data, 0xff, sizeof(data)); ok &= nos_pread(fd, data, 8, 0) == 8;
    ok &= data[0] == 'a' && data[2] == 'c'; for (unsigned i = 3; i < 8; i++) ok &= !data[i];
    ok &= nos_ftruncate(fd, 513) == -27 && !nos_stat64_fd(fd, &st) && st.size == 8;
    ok &= !nos_fchmod(fd, 0600) && !nos_stat64_fd(fd, &st) && (st.mode & 0777) == 0600;
    int ro = nos_open("/tmp/meta", NOS_O_RDONLY); ok &= ro >= 0 && nos_ftruncate(ro, 0) == -NOS_EBADF;
    if (ro >= 0) nos_close(ro);
    nos_check(ok, "vfs_mode_umask_truncate");
    ok = !nos_link_at(NOS_AT_FDCWD, "/tmp/meta", NOS_AT_FDCWD, "/tmp/hard");
    ok &= !nos_stat_at(NOS_AT_FDCWD, "/tmp/hard", 0, &linked) && !nos_stat64_fd(fd, &st) && linked.ino == st.ino && st.nlink == 2;
    ok &= !nos_unlink("/tmp/meta") && !nos_stat64_fd(fd, &st) && st.nlink == 1;
    ok &= !nos_symlink_at("hard", NOS_AT_FDCWD, "/tmp/sym");
    ok &= nos_readlink_at(NOS_AT_FDCWD, "/tmp/sym", data, sizeof(data)) == 4 && data[0] == 'h' && data[3] == 'd';
    ok &= !nos_stat_at(NOS_AT_FDCWD, "/tmp/sym", NOS_AT_SYMLINK_NOFOLLOW, &linked) && (linked.mode & 0170000) == 0120000 && linked.size == 4;
    int symlink = nos_open("/tmp/sym", 0); ok &= symlink >= 0 && nos_read(symlink, data, 3) == 3 && data[2] == 'c';
    if (symlink >= 0) nos_close(symlink);
    ok &= nos_open("/tmp/sym", NOS_O_NOFOLLOW) == -40;
    ok &= !nos_symlink_at("loop2", NOS_AT_FDCWD, "/tmp/loop1") && !nos_symlink_at("loop1", NOS_AT_FDCWD, "/tmp/loop2");
    ok &= nos_open("/tmp/loop1", 0) == -40;
    ok &= !nos_mkdir("/tmp/a") && !nos_mkdir("/tmp/b");
    int probe = nos_open("/tmp/b/probe", NOS_O_CREAT | NOS_O_RDWR); if (probe >= 0) nos_close(probe); else ok = 0;
    ok &= !nos_symlink_at("/tmp/b", NOS_AT_FDCWD, "/tmp/a/l");
    probe = nos_open("/tmp/a/l/../b/probe", 0); ok &= probe >= 0; if (probe >= 0) nos_close(probe);
    ok &= nos_open("/tmp/b/probe/", 0) == -NOS_ENOTDIR;
    nos_unlink("/tmp/a/l"); nos_unlink("/tmp/b/probe"); remove_dir("/tmp/a"); remove_dir("/tmp/b");
    nos_check(ok, "vfs_hardlink_symlink_identity");
    nos_unlink("/tmp/loop1"); nos_unlink("/tmp/loop2"); nos_unlink("/tmp/sym"); nos_unlink("/tmp/hard");
    ok = !nos_stat64_fd(fd, &st) && !st.nlink && nos_truncate("/bin/hello", 0) == -30;
    ok &= !nos_close(fd); nos_umask((unsigned)old_mask);
    ok &= !nos_mkdirat(NOS_AT_FDCWD, "/tmp/dirfd", 0700);
    int dir = nos_open("/tmp/dirfd", NOS_O_DIRECTORY);
    fd = nos_openat_mode(dir, "before", NOS_O_CREAT | NOS_O_RDWR, 0600);
    ok &= dir >= 0 && fd >= 0; if (fd >= 0) nos_close(fd);
    struct nos_message message = { .op = NOS_VFS_RENAMEAT, .arg = dir, .value = dir, .len = 13 };
    u_memcpy(message.data, "before\0after", 13); ok &= !nos_rpc(&message);
    message = (struct nos_message){ .op = NOS_VFS_UNLINKAT, .arg = dir, .len = 6 };
    u_memcpy(message.data, "after", 6); ok &= !nos_rpc(&message);
    if (dir >= 0) nos_close(dir);
    ok &= !remove_dir("/tmp/dirfd"); nos_check(ok, "vfs_dirfd_rename_unlink");
}

static void nos_archive_regression(void)
{
    struct nos_message m; struct nos_stat64 stat;
    int fd = nos_open("/bin/hello", NOS_O_RDONLY);
    int ok = fd >= 0 && !request(NOS_VFS_STAT64, fd, 0);
    m = (struct nos_message){ .op = NOS_VFS_STAT64, .arg = fd };
    ok &= !nos_rpc(&m) && m.len == sizeof(stat); u_memcpy(&stat, m.data, sizeof(stat));
    ok &= stat.size > 512;
    uint64_t offset = 0; m = (struct nos_message){ .op = NOS_VFS_PREAD, .arg = fd, .len = 64 }; u_memcpy(m.data, &offset, 8);
    ok &= nos_rpc(&m) == 64 && m.data[0] == 0x7f && m.data[1] == 'E' && m.data[2] == 'L' && m.data[3] == 'F';
    char header[4]; ok &= nos_read(fd, header, 4) == 4 && header[0] == 0x7f;
    offset = stat.size + 1; m = (struct nos_message){ .op = NOS_VFS_PREAD, .arg = fd, .len = 1 }; u_memcpy(m.data, &offset, 8);
    ok &= !nos_rpc(&m) && nos_seek(fd, 0, 1) == 4 && !nos_close(fd) && nos_open("/bin/hello", NOS_O_RDWR) == -30;
    ok &= nos_open("/bin/hello", NOS_O_RDONLY | NOS_O_DIRECTORY) == -NOS_ENOTDIR;
    nos_check(ok, "vfs_archive_large_readonly");
    int directory = nos_open("/bin", NOS_O_DIRECTORY);
    ok = directory >= 0 && !archive_request(NOS_VFS_CHDIR, 0, "/bin", 0, &m);
    m = (struct nos_message){ .op = NOS_VFS_GETCWD }; ok &= nos_rpc(&m) == 5 && u_streq(m.data, "/bin");
    ok &= !archive_request(NOS_VFS_STATAT, directory, "hello", 0, &m) && m.len == sizeof(stat);
    int relative = archive_request(NOS_VFS_OPENAT, 0, "hello", directory, &m);
    ok &= relative >= 0; if (relative >= 0) nos_close(relative);
    ok &= !archive_request(NOS_VFS_CHDIR, 0, "/", 0, &m);
    nos_check(ok, "vfs_openat_cwd_stat");
    unsigned saw_hello = 0;
    for (unsigned i = 0; i < 8; i++) {
        m = (struct nos_message){ .op = NOS_VFS_GETDENTS, .arg = directory, .len = 64 };
        uint64_t capacity = 88; u_memcpy(m.data + 8, &capacity, 8);
        intptr_t total = nos_rpc(&m); if (!total) break;
        if (total < 20 || total > 88 || m.len != (size_t)total) { ok = 0; break; }
        if (u_streq(m.data + 19, "hello")) saw_hello = 1;
    }
    ok &= saw_hello && !nos_close(directory);
    directory = nos_open("/", NOS_O_DIRECTORY); unsigned fragmented = 0;
    for (unsigned i = 0; i < 40; i++) {
        m = (struct nos_message){ .op = NOS_VFS_GETDENTS, .arg = directory, .len = 64 };
        uint64_t capacity = 88; u_memcpy(m.data + 8, &capacity, 8); intptr_t total = nos_rpc(&m); if (!total) break;
        if (total < 0 || total > 88) { ok = 0; break; }
        if (total > 64) {
            char record[88]; size_t got = m.len; uint64_t cookie = (uintptr_t)m.arg; u_memcpy(record, m.data, got);
            m = (struct nos_message){ .op = NOS_VFS_GETDENTS, .arg = directory, .value = (intptr_t)got, .len = 64 }; u_memcpy(m.data, &cookie, 8);
            ok &= nos_rpc(&m) == total && got + m.len == (size_t)total;
            u_memcpy(record + got, m.data, m.len); fragmented = u_strlen(record + 19) == 62;
        }
    }
    ok &= fragmented && !nos_close(directory); nos_check(ok, "vfs_archive_getdents_fragment");
    fd = nos_open("/tmp/positioned", NOS_O_CREAT | NOS_O_RDWR | NOS_O_CLOEXEC);
    ok = fd >= 0 && nos_write(fd, "abcdef", 6) == 6 && nos_seek(fd, 2, 0) == 2;
    offset = 1; m = (struct nos_message){ .op = NOS_VFS_PREAD, .arg = fd, .len = 4 }; u_memcpy(m.data, &offset, 8);
    ok &= nos_rpc(&m) == 4 && m.data[0] == 'b' && m.data[3] == 'e' && nos_seek(fd, 0, 1) == 2;
    offset = 3; m = (struct nos_message){ .op = NOS_VFS_PWRITE, .arg = fd, .len = 10 }; u_memcpy(m.data, &offset, 8); u_memcpy(m.data + 8, "XY", 2);
    ok &= nos_rpc(&m) == 2 && nos_seek(fd, 0, 1) == 2;
    nos_check(ok, "vfs_positional_io_offsets");
    ok = request(30, fd, 0) == 1;
    m = (struct nos_message){ .op = NOS_VFS_DUP, .arg = fd, .value = -1 }; int plain = (int)nos_rpc(&m);
    ok &= plain >= 0 && !request(30, plain, 0); if (plain >= 0) nos_close(plain);
    m = (struct nos_message){ .op = NOS_VFS_DUP, .arg = fd, .value = -1 }; m.data[0] = 1; int cloexec = (int)nos_rpc(&m);
    ok &= cloexec >= 0 && request(30, cloexec, 0) == 1; if (cloexec >= 0) nos_close(cloexec);
    nos_close(fd); nos_unlink("/tmp/positioned"); nos_check(ok, "vfs_descriptor_cloexec");
    unsigned char entropy[32]; m = (struct nos_message){ .op = NOS_VFS_GETRANDOM, .len = 32 };
    ok = nos_rpc(&m) == 32 && m.len == 32; u_memcpy(entropy, m.data, sizeof(entropy));
    m = (struct nos_message){ .op = NOS_VFS_GETRANDOM, .len = 32 };
    ok &= nos_rpc(&m) == 32 && m.len == 32; unsigned difference = 0;
    for (unsigned i = 0; i < 32; i++) difference |= entropy[i] ^ (unsigned char)m.data[i];
    nos_check(ok && difference, "vfs_getrandom_device");
}

#ifdef NOS_LINUX_COMPAT
static intptr_t file_handle(int fd, unsigned prot)
{ struct nos_message request = { .op = NOS_VFS_SHARED_FILE, .arg = fd, .value = prot }; return nos_rpc(&request); }
static intptr_t shared_mapping(uintptr_t handle, unsigned prot)
{
    struct nos_shared_request request = { .handle = handle, .length = 8192, .prot = prot, .flags = 1 };
    return nos_syscall(NOS_SYS_SHARED, NOS_SHARED_MAP, (uintptr_t)&request, 0);
}
static void shared_regression(void)
{
    int fd = nos_open("/dev/shm/shared", NOS_O_CREAT | NOS_O_RDWR);
    int ok = fd >= 0 && !nos_ftruncate(fd, 8192);
    intptr_t handle = file_handle(fd, 3), first = handle < 0 ? handle : shared_mapping((uintptr_t)handle, 3);
    handle = file_handle(fd, 3);
    intptr_t second = handle < 0 ? handle : shared_mapping((uintptr_t)handle, 3);
    ok &= !nos_pointer_is_error(first) && !nos_pointer_is_error(second) && first != second;
    if (!nos_pointer_is_error(first) && !nos_pointer_is_error(second)) {
        volatile unsigned char *a = (void *)(uintptr_t)first, *b = (void *)(uintptr_t)second;
        struct nos_stat64 old_inode, new_inode; ok &= !nos_stat64_fd(fd, &old_inode);
        a[4097] = 0x5a; char value = 0;
        ok &= b[4097] == 0x5a && nos_pread(fd, &value, 1, 4097) == 1 && value == 0x5a;
        ok &= nos_pwrite(fd, "Q", 1, 5000) == 1 && a[5000] == 'Q' && b[5000] == 'Q';
        ok &= !nos_unlink("/dev/shm/shared") && !nos_close(fd);
        a[4097] = 0x6b; ok &= b[4097] == 0x6b;
        int replacement = nos_open("/dev/shm/shared", NOS_O_CREAT | NOS_O_RDWR);
        ok &= replacement >= 0 && !nos_ftruncate(replacement, 8192) && !nos_stat64_fd(replacement, &new_inode) && old_inode.ino != new_inode.ino;
        intptr_t fresh_handle = file_handle(replacement, 3), fresh = fresh_handle < 0 ? fresh_handle : shared_mapping((uintptr_t)fresh_handle, 3);
        ok &= !nos_pointer_is_error(fresh);
        if (!nos_pointer_is_error(fresh)) {
            volatile unsigned char *c = (void *)(uintptr_t)fresh; ok &= !c[4097]; c[4097] = 0x3d; ok &= b[4097] == 0x6b;
            ok &= !nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)fresh, 8192, 0);
        }
        ok &= !nos_unlink("/dev/shm/shared") && !nos_close(replacement);
        ok &= !nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)first, 8192, 0);
        b[4097] = 0x7c; ok &= b[4097] == 0x7c;
        ok &= !nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)second, 8192, 0);
    } else { if (fd >= 0) nos_close(fd); nos_unlink("/dev/shm/shared"); }
    nos_check(ok, "vfs_shared_file_same_pages_unlink");
    fd = nos_open("/dev/shm/readonly", NOS_O_CREAT | NOS_O_RDWR);
    ok = fd >= 0 && nos_write(fd, "R", 1) == 1;
    handle = file_handle(fd, 1); first = handle < 0 ? handle : shared_mapping((uintptr_t)handle, 1);
    ok &= !nos_pointer_is_error(first);
    if (!nos_pointer_is_error(first)) {
        ok &= !nos_syscall(NOS_SYS_MPROTECT, (uintptr_t)first, 3, 8192);
        ok &= !nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)first, 8192, 0);
    }
    ok &= !nos_close(fd);
    fd = nos_open("/dev/shm/readonly", NOS_O_WRONLY);
    ok &= fd >= 0 && file_handle(fd, 0) == -NOS_EACCES; if (fd >= 0) nos_close(fd);
    fd = nos_open("/dev/shm/readonly", NOS_O_RDONLY);
    ok &= fd >= 0 && file_handle(fd, 3) == -NOS_EACCES;
    handle = file_handle(fd, 1);
    ok &= handle >= 0 && shared_mapping((uintptr_t)handle, 3) == -NOS_EACCES;
    handle = file_handle(fd, 1);
    first = handle < 0 ? handle : shared_mapping((uintptr_t)handle, 1);
    ok &= !nos_pointer_is_error(first);
    if (!nos_pointer_is_error(first)) {
        intptr_t rc = nos_syscall(NOS_SYS_MPROTECT, (uintptr_t)first, 3, 8192);
        ok &= rc == -NOS_EACCES || rc == -NOS_EPERM;
        ok &= !nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)first, 8192, 0);
    }
    if (fd >= 0) nos_close(fd);
    nos_unlink("/dev/shm/readonly");
    nos_check(ok, "vfs_shared_file_readonly_grant");
}
#endif

#ifdef NOS_LINUX_COMPAT
struct long_path_worker { char path[NOS_PATH_MAX]; char marker; };
static void *long_path_worker(void *argument)
{
    struct long_path_worker *worker = argument;
    for (unsigned i = 0; i < 20; i++) {
        int fd = nos_open(worker->path, NOS_O_CREAT | NOS_O_RDWR | NOS_O_TRUNC);
        char byte = 0;
        if (fd < 0 || nos_write(fd, &worker->marker, 1) != 1 || nos_seek(fd, 0, 0) ||
            nos_read(fd, &byte, 1) != 1 || byte != worker->marker || nos_close(fd)) return (void *)1;
    }
    return 0;
}
static void path_suffix(char output[NOS_PATH_MAX], const char *parent, const char *name)
{
    size_t count = u_strlen(parent); u_memcpy(output, parent, count); output[count++] = '/';
    u_memcpy(output + count, name, u_strlen(name) + 1);
}
static void long_path_regression(void)
{
    char archive[NOS_PATH_MAX], deep[NOS_PATH_MAX] = "/tmp", parents[4][NOS_PATH_MAX];
    size_t at = 0;
    for (unsigned component = 0; component < 4; component++) {
        archive[at++] = '/'; for (unsigned i = 0; i < 50; i++) archive[at++] = (char)('a' + component);
    }
    u_memcpy(archive + at, "/hello", 7);
    struct nos_stat64 st; char elf[4]; int fd = nos_open(archive, 0);
    int ok = fd >= 0 && !nos_stat64_fd(fd, &st) && st.size > 512 && nos_read(fd, elf, 4) == 4 && elf[0] == 0x7f && elf[1] == 'E';
    if (fd >= 0) nos_close(fd);
    for (unsigned component = 0; component < 4; component++) {
        size_t end = u_strlen(deep); deep[end++] = '/';
        for (unsigned i = 0; i < 50; i++) deep[end++] = (char)('A' + component);
        deep[end] = 0; u_memcpy(parents[component], deep, end + 1);
        ok &= !nos_mkdirat(NOS_AT_FDCWD, deep, 0700);
    }
    int directory = nos_open(deep, NOS_O_DIRECTORY);
    fd = nos_openat_mode(directory, "before", NOS_O_CREAT | NOS_O_RDWR, 0600);
    ok &= directory >= 0 && fd >= 0 && nos_write(fd, "long", 4) == 4; if (fd >= 0) nos_close(fd);
    nos_check(ok, "vfs_long_path_openat_archive");
    char before[NOS_PATH_MAX], after[NOS_PATH_MAX], hard[NOS_PATH_MAX], link[NOS_PATH_MAX], output[NOS_PATH_MAX];
    path_suffix(before, deep, "before"); path_suffix(after, deep, "after"); path_suffix(hard, deep, "hard"); path_suffix(link, deep, "symbolic");
    ok = !nos_renameat(NOS_AT_FDCWD, before, NOS_AT_FDCWD, after) && !nos_link_at(NOS_AT_FDCWD, after, NOS_AT_FDCWD, hard);
    ok &= !nos_symlink_at(after, NOS_AT_FDCWD, link);
    ok &= nos_readlink_at(NOS_AT_FDCWD, link, output, sizeof(output)) == (intptr_t)u_strlen(after);
    output[u_strlen(after)] = 0; ok &= u_streq(output, after);
    ok &= nos_resolve_at(NOS_AT_FDCWD, link, output, sizeof(output)) == (intptr_t)(u_strlen(after) + 1) && u_streq(output, after);
    ok &= !nos_chdir(deep) && nos_getcwd(output, sizeof(output)) == (intptr_t)(u_strlen(deep) + 1) && u_streq(output, deep);
    ok &= nos_getcwd(output, 8) == -NOS_ERANGE;
    fd = nos_open("after", 0); ok &= fd >= 0 && nos_read(fd, elf, 4) == 4 && elf[0] == 'l'; if (fd >= 0) nos_close(fd);
    ok &= !nos_chdir("/");
    ok &= !nos_unlinkat(directory, "symbolic", 0) && !nos_unlinkat(NOS_AT_FDCWD, hard, 0) && !nos_unlinkat(NOS_AT_FDCWD, after, 0);
    nos_check(ok, "vfs_long_path_pair_cwd_resolve");
    char maximum[NOS_PATH_MAX + 1]; maximum[0] = '/';
    for (unsigned i = 1; i < NOS_PATH_MAX - 1; i++) maximum[i] = 'z';
    maximum[NOS_PATH_MAX - 1] = 0;
    fd = nos_open(maximum, NOS_O_CREAT | NOS_O_RDWR); ok = fd >= 0; if (fd >= 0) nos_close(fd);
    int root = nos_open("/", NOS_O_DIRECTORY); unsigned seen = 0;
    unsigned char record[NOS_DIRENT_RECORD_MAX];
    for (unsigned i = 0; root >= 0 && i < 50; i++) {
        intptr_t bytes = nos_getdents(root, record, sizeof(record)); if (!bytes) break;
        if (bytes < 24 || bytes > (intptr_t)sizeof(record)) { ok = 0; break; }
        size_t offset = 0;
        while (offset < (size_t)bytes) {
            uint16_t length; u_memcpy(&length, record + offset + 16, 2);
            if (length < 24 || offset + length > (size_t)bytes) { ok = 0; break; }
            if (u_streq((char *)record + offset + 19, maximum + 1)) { seen = 1; ok &= length == 280; }
            offset += length;
        }
    }
    ok &= seen; if (root >= 0) nos_close(root); ok &= !nos_unlink(maximum);
    maximum[NOS_PATH_MAX - 1] = 'z'; maximum[NOS_PATH_MAX] = 0;
    ok &= nos_open(maximum, NOS_O_CREAT | NOS_O_RDWR) == -NOS_ENAMETOOLONG;
    nos_check(ok, "vfs_long_dirent_boundary");
    struct long_path_worker work[2]; struct nos_thread threads[2];
    path_suffix(work[0].path, deep, "thread0"); path_suffix(work[1].path, deep, "thread1"); work[0].marker = '0'; work[1].marker = '1';
    int created[2]; created[0] = nos_thread_create(&threads[0], long_path_worker, &work[0]); created[1] = nos_thread_create(&threads[1], long_path_worker, &work[1]);
    ok = !created[0] && !created[1];
    for (unsigned i = 0; i < 2; i++) if (!created[i]) { void *result; ok &= !nos_thread_join(&threads[i], &result) && !result; }
    ok &= !nos_unlink(work[0].path) && !nos_unlink(work[1].path);
    struct nos_message bad = { .op = NOS_VFS_OPEN, .arg = NOS_O_CREAT | NOS_O_RDWR, .bulk_in = 1, .bulk_in_len = NOS_PATH_MAX };
    ok &= nos_rpc(&bad) == -NOS_EFAULT;
    fd = nos_openat_mode(directory, "valid-after-bad-span", NOS_O_CREAT | NOS_O_RDWR, 0600);
    ok &= fd >= 0; if (fd >= 0) nos_close(fd);
    ok &= !nos_unlinkat(directory, "valid-after-bad-span", 0);
    if (directory >= 0) nos_close(directory);
    for (unsigned i = 4; i; i--) ok &= !nos_unlinkat(NOS_AT_FDCWD, parents[i - 1], 0x200);
    nos_check(ok, "vfs_long_path_thread_isolation");
}
#endif

void nos_persistence_regression(unsigned mode)
{
#ifdef NOS_LINUX_COMPAT
    if (mode == 9) { long_path_regression(); return; }
    if (mode == 8) { shared_regression(); return; }
#endif
    if (mode == 7) { nos_archive_regression(); metadata_regression(); return; }
    if (mode == 6) { nos_vfs_regression(); return; }
    struct nos_message m = { .op = NOS_VFS_MOUNT, .arg = mode == 3 ? 3 : mode == 4 ? 2 : 1, .len = 9 };
    u_memcpy(m.data, "/persist", 9);
    int ok = !nos_rpc(&m);
    if (mode == 1 || mode == 3 || mode == 5) {
        int fd = nos_open("/persist/state", NOS_O_CREAT | NOS_O_RDWR | NOS_O_TRUNC);
        ok &= fd >= 3 && nos_write(fd, "NOS persistent block", 20) == 20 && !nos_close(fd) && !request(NOS_VFS_SYNC, 0, 0);
        if (mode == 5) {
            fd = nos_open("/persist/state", NOS_O_RDWR | NOS_O_TRUNC);
            ok &= fd >= 3 && nos_write(fd, "newer generation", 16) == 16 && !nos_close(fd) && !request(NOS_VFS_SYNC, 0, 0);
        }
        nos_check(ok, mode == 5 ? "vfs_persistent_two_generations" : mode == 3 ? "vfs_fat_write" : "vfs_persistent_write");
    } else {
        char buffer[32] = { 0 }; int fd = nos_open("/persist/state", NOS_O_RDONLY);
        ok &= fd >= 3 && nos_read(fd, buffer, sizeof(buffer)) == 20 && u_streq(buffer, "NOS persistent block") && !nos_close(fd);
        nos_check(ok, mode == 4 ? "vfs_fat_read" : "vfs_persistent_read");
    }
    m = (struct nos_message){ .op = NOS_VFS_DEVICE_STATS };
    struct nos_block_stats stats = { 0 };
    ok = !nos_rpc(&m) && m.len == sizeof(stats);
    if (ok) u_memcpy(&stats, m.data, sizeof(stats));
    nos_check(ok && stats.requests && stats.interrupts >= stats.requests && !stats.poll_fallbacks, "virtio_hardware_irq");
    nos_check(ok && (stats.version == 1 || stats.version == 2), "virtio_transport_version");
}
