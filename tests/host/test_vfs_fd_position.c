/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include "../../fs/vfs_fd.h"

uintptr_t kernel_running = 1;
static atomic_int seen[100];

static ssize_t delayed_read(struct file *file, char *buf, size_t count, loff_t *pos)
{
    loff_t old = *pos;
    (void)file;
    assert(count >= 1 && old >= 0 && old < 100);
    usleep(1000);
    *buf = (char)old;
    *pos = old + 1;
    return 1;
}

static int test_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    (void)file;
    if (cmd == 0x1234) {
        *(int *)(uintptr_t)arg = 42;
        return 0;
    }
    return -EINVAL;
}

static const struct file_operations ops = {
    .read = delayed_read,
    .ioctl = test_ioctl,
};

int vfs_get_mount_count(void)
{
    return 0;
}

int vfs_get_mount_entry(int index, struct vfs_mount_entry *entry)
{
    (void)index;
    (void)entry;
    return -ENOENT;
}

static void *reader(void *arg)
{
    int fd = *(int *)arg;
    for (int i = 0; i < 50; i++) {
        char value;
        assert(nos_read(fd, &value, 1) == 1);
        atomic_fetch_add(&seen[(unsigned char)value], 1);
    }
    return NULL;
}

int main(void)
{
    struct file *file = calloc(1, sizeof(*file));
    pthread_t a, b;
    int fd = vfs_fd_reserve();
    assert(file != NULL && fd >= 0);
    file->f_op = &ops;
    file->f_flags = O_RDONLY;
    vfs_fd_publish(fd, file);
    assert(pthread_create(&a, NULL, reader, &fd) == 0);
    assert(pthread_create(&b, NULL, reader, &fd) == 0);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    for (int i = 0; i < 100; i++) assert(atomic_load(&seen[i]) == 1);
    assert(nos_lseek(fd, 0, SEEK_CUR) == 100);
    assert(nos_lseek(fd, LLONG_MAX, SEEK_CUR) == -EINVAL);
    assert(nos_lseek(fd, 0, SEEK_CUR) == 100);
    int ioctl_val = 0;
    assert(nos_ioctl(fd, 0x1234, (unsigned long)(uintptr_t)&ioctl_val) == 0);
    assert(ioctl_val == 42);
    assert(nos_ioctl(fd, 0x9999, 0) == -EINVAL);
    assert(nos_ioctl(-1, 0x1234, 0) == -EBADF);
    assert(vfs_fd_close(fd) == 0);
    return 0;
}
