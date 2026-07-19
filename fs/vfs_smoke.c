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

    pr_info("vfs_smoke OK\r\n");
    return 0;
}

SYS_INIT(vfs_smoke, INIT_LEVEL_APPLICATION);
