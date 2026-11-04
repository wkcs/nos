/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <fs/fs.h>
#include <fs/vfs.h>
#include <kernel/kernel.h>
#include <kernel/mutex.h>
#include "vfs_fd.h"

/* Before scheduling begins there can be no competing task. */
static int file_lock_position(struct file *file, bool *locked)
{
    *locked = false;
    if (!kernel_running) return 0;
    mutex_lock(&file->f_pos_lock);
    *locked = true;
    return 0;
}

static void file_unlock_position(struct file *file, bool locked)
{
    if (locked) mutex_unlock(&file->f_pos_lock);
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

/**
 * nos_fsync - 将指定文件描述符的脏数据与元数据刷新到存储介质
 * @fd: 目标文件描述符
 *
 * 成功返回 0，失败返回负值错误码（如 -EBADF, -EIO 等）。
 */
int nos_fsync(int fd)
{
    struct file *file;
    int ret = 0;

    file = vfs_fd_get(fd);
    if (file == NULL) return -EBADF;

    if (file->f_op != NULL && file->f_op->fsync != NULL) {
        ret = file->f_op->fsync(file, 0, (loff_t)-1, 0);
    } else if (file->f_path.dentry != NULL &&
               file->f_path.dentry->d_sb != NULL &&
               file->f_path.dentry->d_sb->s_op != NULL &&
               file->f_path.dentry->d_sb->s_op->sync_fs != NULL) {
        ret = file->f_path.dentry->d_sb->s_op->sync_fs(file->f_path.dentry->d_sb, 1);
    }

    (void)vfs_fd_put(file);
    return ret;
}

/**
 * nos_fdatasync - 将指定文件描述符的数据刷新到存储介质（元数据在不影响后续读取时可不强制刷新）
 * @fd: 目标文件描述符
 *
 * 在当前轻量实现中直接委托给 nos_fsync(fd)。
 */
int nos_fdatasync(int fd)
{
    return nos_fsync(fd);
}

/**
 * nos_sync - 遍历所有已挂载文件系统超级块并触发持久化刷盘
 *
 * 成功返回 0，如遇错误返回首个失败的错误码。
 */
int nos_sync(void)
{
    int count = vfs_get_mount_count();
    struct super_block *synced[16];
    int synced_count = 0;
    int ret = 0;

    for (int i = 0; i < count; i++) {
        struct vfs_mount_entry entry;
        if (vfs_get_mount_entry(i, &entry) != 0 || entry.dentry == NULL) {
            continue;
        }
        struct super_block *sb = entry.dentry->d_sb;
        if (sb == NULL || sb->s_op == NULL || sb->s_op->sync_fs == NULL) {
            continue;
        }
        bool already_synced = false;
        for (int j = 0; j < synced_count; j++) {
            if (synced[j] == sb) {
                already_synced = true;
                break;
            }
        }
        if (already_synced) {
            continue;
        }
        if (synced_count < (int)(sizeof(synced) / sizeof(synced[0]))) {
            synced[synced_count++] = sb;
        }
        int err = sb->s_op->sync_fs(sb, 1);
        if (err != 0 && ret == 0) {
            ret = err;
        }
    }
    return ret;
}
