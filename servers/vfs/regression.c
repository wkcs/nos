/* Copyright (C) 2026 NOS contributors */
#include "abi.h"
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
    char bytes[8]; int fd = nos_open("/tmp/p0", NOS_O_CREAT | NOS_O_RDWR), held[8], count = 0;
    int ok = fd >= 3 && nos_write(fd, "SECRET", 6) == 6;
    while (count < 8) { int extra = nos_open("/tmp/p0", NOS_O_RDONLY); if (extra < 0) break; held[count++] = extra; }
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
void nos_persistence_regression(unsigned mode)
{
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
