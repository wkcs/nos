/**
 * Copyright (C) 2026 Nick Hu
 *
 * Author: Nick Hu
 */

#include <fs/fs.h>
#include <fs/vfs.h>
#include <kernel/init.h>
#include <kernel/printk.h>
#include <kernel/task.h>
#include <string.h>

#if defined(CONFIG_FATFS) && defined(CONFIG_BOARD_QEMU_STM32)
static int check_fatfs_statfs(struct dentry *root)
{
    struct kstatfs before, written, removed;
    const struct super_operations *ops;
    struct vfs_mount_info info;
    int fd;
    ssize_t count;

    if (root == NULL || root->d_sb == NULL) {
        return -EIO;
    }
    ops = root->d_sb->s_op;
    if (vfs_get_mount_info(0, &info) != 0 || strcmp(info.mount_path, "/") != 0 ||
        strcmp(info.fs_name, "fatfs") != 0 || !info.has_statfs ||
        ops == NULL || ops->statfs == NULL || ops->statfs(root, &before) != 0 ||
        before.bsize == 0U || before.bfree == 0U || before.bfree > before.blocks) {
        return -EIO;
    }
    fd = nos_open("/statfs.tmp", O_CREAT | O_EXCL | O_WRONLY);
    if (fd < 0) {
        return fd;
    }
    count = nos_write(fd, "x", 1);
    if (nos_close(fd) != 0 || count != 1 ||
        ops->statfs(root, &written) != 0 || written.blocks != before.blocks ||
        written.bfree != before.bfree - 1U || written.bavail != written.bfree ||
        nos_unlink("/statfs.tmp") != 0 || ops->statfs(root, &removed) != 0 ||
        removed.bfree != before.bfree) {
        return -EIO;
    }
    return 0;
}
#endif

static int vfs_smoke(void)
{
    static const char expected[] = "nos\n";
    char actual[sizeof(expected)];
    ssize_t count;
    int fd;

    fd = nos_open("/smoke.txt", O_CREAT | O_RDWR);
    if (fd < 0) {
        pr_err("vfs_smoke: open for write failed: %d\n", fd);
        return fd;
    }

    count = nos_write(fd, expected, sizeof(expected) - 1);
    if (count != (ssize_t)(sizeof(expected) - 1)) {
        pr_err("vfs_smoke: write failed: %d\n", (int)count);
        nos_close(fd);
        return -EIO;
    }

    /* 验证 nos_fsync / nos_fdatasync 与 nos_sync 系统调用契约 */
    if (nos_fsync(fd) != 0 || nos_fdatasync(fd) != 0) {
        pr_err("vfs_smoke: fsync/fdatasync on open file failed\n");
        nos_close(fd);
        return -EIO;
    }
    if (nos_fsync(-1) != -EBADF || nos_fdatasync(-1) != -EBADF) {
        pr_err("vfs_smoke: fsync on invalid fd failed\n");
        nos_close(fd);
        return -EIO;
    }
    if (nos_sync() != 0) {
        pr_err("vfs_smoke: nos_sync failed\n");
        nos_close(fd);
        return -EIO;
    }

    if (nos_close(fd) < 0) {
        pr_err("vfs_smoke: close after write failed\n");
        return -EIO;
    }

    fd = nos_open("/smoke.txt", O_RDONLY);
    if (fd < 0) {
        pr_err("vfs_smoke: open for read failed: %d\n", fd);
        return fd;
    }

    memset(actual, 0, sizeof(actual));
    count = nos_read(fd, actual, sizeof(expected) - 1);
    if (nos_close(fd) < 0 || count != (ssize_t)(sizeof(expected) - 1) ||
        memcmp(actual, expected, sizeof(expected) - 1) != 0) {
        pr_err("vfs_smoke: readback mismatch\n");
        return -EIO;
    }
    if (nos_rename("/smoke.txt", "/smoke.txt") != 0 ||
        nos_rename("/vfs-no-such-file", "/vfs-no-such-file") != -ENOENT) {
        pr_err("vfs_smoke: same-path rename failed\n");
        return -EIO;
    }
    if (nos_mkdir("/unlink-dir", 0755) != 0 ||
        nos_unlink("/unlink-dir") != -EISDIR ||
        nos_rmdir("/unlink-dir") != 0) {
        pr_err("vfs_smoke: unlink accepted a directory\n");
        return -EIO;
    }
    fd = nos_open("/typecheck.tmp", O_CREAT | O_RDWR);
    if (fd < 0 || nos_close(fd) != 0 || nos_rmdir("/typecheck.tmp") != -ENOTDIR ||
        nos_unlink("/typecheck.tmp") != 0) {
        pr_err("vfs_smoke: rmdir accepted a file\n");
        return -EIO;
    }

    if (nos_mkdir("/cycle", 0755) != 0 ||
        nos_mkdir("/cycle/sub", 0755) != 0 ||
        nos_rename("/cycle", "/cycle/moved") != -EINVAL ||
        nos_rename("/cycle", "/cycle/sub/moved") != -EINVAL ||
        nos_rename("/cycle/sub", "/moved") != 0 ||
        nos_rename("/moved", "/cycle/sub") != 0 ||
        nos_rmdir("/cycle/sub") != 0 || nos_rmdir("/cycle") != 0) {
        pr_err("vfs_smoke: rename into descendant failed\n");
        return -EIO;
    }

    fd = nos_open("/smoke.txt", O_RDONLY);
    if (fd < 0) return fd;
    count = nos_write(fd, "x", 1);
    nos_close(fd);
    if (count != -EBADF) {
        pr_err("vfs_smoke: readonly descriptor allowed write: %d\n", (int)count);
        return -EIO;
    }
    fd = nos_open("/smoke.txt", O_WRONLY);
    if (fd < 0) return fd;
    count = nos_read(fd, actual, 1);
    nos_close(fd);
    if (count != -EBADF || nos_open("/smoke.txt", O_RDONLY | O_TRUNC) != -EINVAL) {
        pr_err("vfs_smoke: invalid descriptor access\n");
        return -EIO;
    }

    {
        int descriptors[CONFIG_TASK_MAX_FDS];
        int i;
        for (i = 0; i < CONFIG_TASK_MAX_FDS; i++) {
            descriptors[i] = nos_open("/sys/arch", O_RDONLY);
            if (descriptors[i] < 0) {
                while (--i >= 0) nos_close(descriptors[i]);
                return -EIO;
            }
        }
        fd = nos_open("/smoke.txt", O_WRONLY | O_TRUNC);
        for (i = 0; i < CONFIG_TASK_MAX_FDS; i++) nos_close(descriptors[i]);
        if (fd != -EMFILE) {
            pr_err("vfs_smoke: expected descriptor exhaustion: %d\n", fd);
            if (fd >= 0) nos_close(fd);
            return -EIO;
        }
        fd = nos_open("/smoke.txt", O_RDONLY);
        if (fd < 0) return fd;
        count = nos_read(fd, actual, sizeof(expected) - 1);
        nos_close(fd);
        if (count != (ssize_t)(sizeof(expected) - 1) ||
            memcmp(actual, expected, sizeof(expected) - 1) != 0) {
            pr_err("vfs_smoke: failed open changed file contents\n");
            return -EIO;
        }
    }

#if defined(CONFIG_FATFS) && defined(CONFIG_BOARD_QEMU_STM32)
    if (check_fatfs_statfs(vfs_get_root()) != 0) {
        pr_err("vfs_smoke: FatFs allocation statistics mismatch\n");
        return -EIO;
    }
#endif

    pr_info("vfs_smoke OK\n");
    return 0;
}

SYS_INIT(vfs_smoke, INIT_LEVEL_APPLICATION);
