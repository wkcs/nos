/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <fs/fs.h>
#include <fs/vfs.h>
#include <kernel/kernel.h>
#include <kernel/sem.h>
#include "vfs_fd.h"

/* Before scheduling begins there can be no competing task. */
static int file_lock_position(struct file *file, bool *locked)
{
    int rc;
    *locked = false;
    if (!kernel_running) return 0;
    rc = sem_get_timeout(&file->f_pos_sem, 0);
    if (rc != 0) return rc;
    *locked = true;
    return 0;
}

static void file_unlock_position(struct file *file, bool locked)
{
    if (locked) sem_send_one(&file->f_pos_sem);
}

ssize_t nos_read(int fd, void *buf, size_t count)
{
    struct file *file;
    ssize_t ret;
    bool locked;

    if (buf == NULL) return -EBADF;
    file = vfs_fd_get(fd);
    if (file == NULL) return -EBADF;
    if ((file->f_flags & O_ACCMODE) == O_WRONLY || file->f_op == NULL ||
        file->f_op->read == NULL) {
        ret = -EBADF;
    } else {
        ret = file_lock_position(file, &locked);
        if (ret == 0) {
            ret = file->f_op->read(file, buf, count, &file->f_pos);
            file_unlock_position(file, locked);
        }
    }
    (void)vfs_fd_put(file);
    return ret;
}

ssize_t nos_write(int fd, const void *buf, size_t count)
{
    struct file *file;
    ssize_t ret;
    bool locked;

    if (buf == NULL) return -EBADF;
    file = vfs_fd_get(fd);
    if (file == NULL) return -EBADF;
    if ((file->f_flags & O_ACCMODE) == O_RDONLY || file->f_op == NULL ||
        file->f_op->write == NULL) {
        ret = -EBADF;
    } else {
        ret = file_lock_position(file, &locked);
        if (ret == 0) {
            if (file->f_flags & O_APPEND) file->f_pos = file->f_inode->i_size;
            ret = file->f_op->write(file, buf, count, &file->f_pos);
            file_unlock_position(file, locked);
        }
    }
    (void)vfs_fd_put(file);
    return ret;
}

loff_t nos_lseek(int fd, loff_t offset, int whence)
{
    struct file *file = vfs_fd_get(fd);
    loff_t base, new_pos;
    bool locked;

    if (file == NULL) return -EBADF;
    new_pos = file_lock_position(file, &locked);
    if (new_pos != 0) {
        (void)vfs_fd_put(file);
        return new_pos;
    }
    if (file->f_op != NULL && file->f_op->llseek != NULL) {
        new_pos = file->f_op->llseek(file, offset, whence);
        goto out;
    }
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = file->f_pos; break;
    case SEEK_END: base = file->f_inode != NULL ? file->f_inode->i_size : 0; break;
    default: new_pos = -EINVAL; goto out;
    }
    if (__builtin_add_overflow(base, offset, &new_pos) || new_pos < 0) {
        new_pos = -EINVAL;
        goto out;
    }
    file->f_pos = new_pos;
out:
    file_unlock_position(file, locked);
    (void)vfs_fd_put(file);
    return new_pos;
}
