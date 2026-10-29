/**
 * Copyright (C) 2026 Nick Hu
 *
 * Author: Nick Hu
 */

#include <fs/fs.h>
#include <kernel/init.h>
#include <kernel/printk.h>
#include <string.h>

static int vfs_smoke(void)
{
    static const char expected[] = "nos\n";
    char actual[sizeof(expected)];
    ssize_t count;
    int fd;

    fd = nos_open("/smoke.txt", O_CREAT | O_RDWR);
    if (fd < 0) {
        pr_err("vfs_smoke: open for write failed: %d\r\n", fd);
        return fd;
    }

    count = nos_write(fd, expected, sizeof(expected) - 1);
    if (count != (ssize_t)(sizeof(expected) - 1)) {
        pr_err("vfs_smoke: write failed: %d\r\n", (int)count);
        nos_close(fd);
        return -EIO;
    }

    if (nos_close(fd) < 0) {
        pr_err("vfs_smoke: close after write failed\r\n");
        return -EIO;
    }

    fd = nos_open("/smoke.txt", O_RDONLY);
    if (fd < 0) {
        pr_err("vfs_smoke: open for read failed: %d\r\n", fd);
        return fd;
    }

    memset(actual, 0, sizeof(actual));
    count = nos_read(fd, actual, sizeof(expected) - 1);
    if (nos_close(fd) < 0 || count != (ssize_t)(sizeof(expected) - 1) ||
        memcmp(actual, expected, sizeof(expected) - 1) != 0) {
        pr_err("vfs_smoke: readback mismatch\r\n");
        return -EIO;
    }
    if (nos_rename("/smoke.txt", "/smoke.txt") != 0 ||
        nos_rename("/vfs-no-such-file", "/vfs-no-such-file") != -ENOENT) {
        pr_err("vfs_smoke: same-path rename failed\r\n");
        return -EIO;
    }
    if (nos_mkdir("/unlink-dir", 0755) != 0 ||
        nos_unlink("/unlink-dir") != -EISDIR ||
        nos_rmdir("/unlink-dir") != 0) {
        pr_err("vfs_smoke: unlink accepted a directory\r\n");
        return -EIO;
    }
    fd = nos_open("/typecheck.tmp", O_CREAT | O_RDWR);
    if (fd < 0 || nos_close(fd) != 0 || nos_rmdir("/typecheck.tmp") != -ENOTDIR ||
        nos_unlink("/typecheck.tmp") != 0) {
        pr_err("vfs_smoke: rmdir accepted a file\r\n");
        return -EIO;
    }

    if (nos_mkdir("/cycle", 0755) != 0 ||
        nos_mkdir("/cycle/sub", 0755) != 0 ||
        nos_rename("/cycle", "/cycle/moved") != -EINVAL ||
        nos_rename("/cycle", "/cycle/sub/moved") != -EINVAL ||
        nos_rename("/cycle/sub", "/moved") != 0 ||
        nos_rename("/moved", "/cycle/sub") != 0 ||
        nos_rmdir("/cycle/sub") != 0 || nos_rmdir("/cycle") != 0) {
        pr_err("vfs_smoke: rename into descendant failed\r\n");
        return -EIO;
    }

    fd = nos_open("/smoke.txt", O_RDONLY);
    if (fd < 0) return fd;
    count = nos_write(fd, "x", 1);
    nos_close(fd);
    if (count != -EBADF) {
        pr_err("vfs_smoke: readonly descriptor allowed write: %d\r\n", (int)count);
        return -EIO;
    }
    fd = nos_open("/smoke.txt", O_WRONLY);
    if (fd < 0) return fd;
    count = nos_read(fd, actual, 1);
    nos_close(fd);
    if (count != -EBADF || nos_open("/smoke.txt", O_RDONLY | O_TRUNC) != -EINVAL) {
        pr_err("vfs_smoke: invalid descriptor access\r\n");
        return -EIO;
    }

    {
        int descriptors[16];
        int i;
        for (i = 0; i < 16; i++) {
            descriptors[i] = nos_open("/sys/arch", O_RDONLY);
            if (descriptors[i] < 0) {
                while (--i >= 0) nos_close(descriptors[i]);
                return -EIO;
            }
        }
        fd = nos_open("/smoke.txt", O_WRONLY | O_TRUNC);
        for (i = 0; i < 16; i++) nos_close(descriptors[i]);
        if (fd != -EMFILE) {
            pr_err("vfs_smoke: expected descriptor exhaustion: %d\r\n", fd);
            if (fd >= 0) nos_close(fd);
            return -EIO;
        }
        fd = nos_open("/smoke.txt", O_RDONLY);
        if (fd < 0) return fd;
        count = nos_read(fd, actual, sizeof(expected) - 1);
        nos_close(fd);
        if (count != (ssize_t)(sizeof(expected) - 1) ||
            memcmp(actual, expected, sizeof(expected) - 1) != 0) {
            pr_err("vfs_smoke: failed open changed file contents\r\n");
            return -EIO;
        }
    }

    pr_info("vfs_smoke OK\r\n");
    return 0;
}

SYS_INIT(vfs_smoke, INIT_LEVEL_APPLICATION);
