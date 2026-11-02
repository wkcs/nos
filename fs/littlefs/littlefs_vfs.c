/**
 * Copyright (C) 2026 胡启航 <huqihan@live.com>
 *
 * Author: 胡启航 <Nick Hu>
 */

#include <fs/fs.h>
#include <fs/vfs.h>
#include <kernel/device.h>
#include <kernel/errno.h>
#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <kernel/mutex.h>
#include <kernel/printk.h>
#include <drivers/block.h>
#include <string.h>

#include "lfs.h"

/* LittleFS 超级块私有信息 */
struct littlefs_sb_info {
    lfs_t lfs;
    struct lfs_config cfg;
    const struct device *bdev;
    uint32_t sector_size;
    uint32_t block_count;
    struct mutex lock;
};

/* 打开文件私有上下文 */
struct littlefs_file {
    struct littlefs_sb_info *sbi;
    lfs_file_t file;
};

/* 打开目录私有上下文 */
struct littlefs_dir {
    struct littlefs_sb_info *sbi;
    lfs_dir_t dir;
};

static struct inode_operations littlefs_dir_inode_ops;
static struct inode_operations littlefs_file_inode_ops;
static struct file_operations littlefs_dir_ops;
static struct file_operations littlefs_file_ops;
static const struct super_operations littlefs_super_ops;

/**
 * lfs_to_errno - 将 LittleFS 错误码转换为系统标准 errno
 */
static int lfs_to_errno(int err)
{
    switch (err) {
    case LFS_ERR_OK:          return 0;
    case LFS_ERR_IO:          return -EIO;
    case LFS_ERR_CORRUPT:     return -EIO;
    case LFS_ERR_NOENT:       return -ENOENT;
    case LFS_ERR_EXIST:       return -EEXIST;
    case LFS_ERR_NOTDIR:      return -ENOTDIR;
    case LFS_ERR_ISDIR:       return -EISDIR;
    case LFS_ERR_NOTEMPTY:    return -ENOTEMPTY;
    case LFS_ERR_BADF:        return -EBADF;
    case LFS_ERR_FBIG:        return -EFBIG;
    case LFS_ERR_INVAL:       return -EINVAL;
    case LFS_ERR_NOSPC:       return -ENOSPC;
    case LFS_ERR_NOMEM:       return -ENOMEM;
    case LFS_ERR_NOATTR:      return -ENODATA;
    case LFS_ERR_NAMETOOLONG: return -ENAMETOOLONG;
    default:                  return -EIO;
    }
}

/**
 * get_littlefs_path - 从 dentry 树回溯重构 LittleFS 绝对路径
 */
static int get_littlefs_path(struct dentry *dentry, char *buf, int size)
{
    char *p;
    struct dentry *root;

    if (dentry == NULL || buf == NULL || size <= 1) {
        return -EINVAL;
    }

    root = dentry->d_sb ? dentry->d_sb->s_root : NULL;
    p = buf + size - 1;
    *p = '\0';

    if (dentry == root || (root != NULL && dentry->d_inode != NULL &&
                           dentry->d_inode == root->d_inode)) {
        if (size < 2) {
            return -ENAMETOOLONG;
        }
        buf[0] = '/';
        buf[1] = '\0';
        return 0;
    }

    while (dentry != NULL && dentry != root &&
           (root == NULL || dentry->d_inode == NULL ||
            dentry->d_inode != root->d_inode)) {
        int len = dentry->d_name.len;
        if (len <= 0) {
            break;
        }
        p -= len;
        if (p <= buf) {
            return -ENAMETOOLONG;
        }
        memcpy(p, dentry->d_name.name, len);
        *--p = '/';
        dentry = dentry->d_parent;
    }

    if (p >= buf + size - 1) {
        if (size < 2) {
            return -ENAMETOOLONG;
        }
        buf[0] = '/';
        buf[1] = '\0';
        return 0;
    }

    int path_len = (buf + size - 1) - p;
    memmove(buf, p, path_len + 1);
    return 0;
}

/**
 * littlefs_make_inode - 创建并初始化 LittleFS 关联的 VFS inode
 */
static struct inode *littlefs_make_inode(struct super_block *sb, int mode)
{
    struct inode *inode = nos_vfs_alloc_inode(sb);
    if (inode == NULL) {
        return NULL;
    }

    inode->i_mode = mode;
    inode->i_size = 0;

    if (mode & S_IFDIR) {
        inode->i_op = &littlefs_dir_inode_ops;
        inode->i_fop = &littlefs_dir_ops;
    } else {
        inode->i_op = &littlefs_file_inode_ops;
        inode->i_fop = &littlefs_file_ops;
    }
    return inode;
}

/*
 * LittleFS 底层块设备驱动回调
 */

static int littlefs_bd_read(const struct lfs_config *c, lfs_block_t block,
                            lfs_off_t off, void *buffer, lfs_size_t size)
{
    struct littlefs_sb_info *sbi = (struct littlefs_sb_info *)c->context;
    uint32_t sector_size = sbi->sector_size ? sbi->sector_size : 512U;
    uint32_t sectors_per_block = c->block_size / sector_size;
    uint32_t lba = block * sectors_per_block + (off / sector_size);
    uint32_t count = size / sector_size;
    int ret;

    ret = block_read(sbi->bdev, lba, buffer, count);
    return ret == 0 ? LFS_ERR_OK : LFS_ERR_IO;
}

static int littlefs_bd_prog(const struct lfs_config *c, lfs_block_t block,
                            lfs_off_t off, const void *buffer, lfs_size_t size)
{
    struct littlefs_sb_info *sbi = (struct littlefs_sb_info *)c->context;
    uint32_t sector_size = sbi->sector_size ? sbi->sector_size : 512U;
    uint32_t sectors_per_block = c->block_size / sector_size;
    uint32_t lba = block * sectors_per_block + (off / sector_size);
    uint32_t count = size / sector_size;
    int ret;

    ret = block_write(sbi->bdev, lba, buffer, count);
    return ret == 0 ? LFS_ERR_OK : LFS_ERR_IO;
}

static int littlefs_bd_erase(const struct lfs_config *c, lfs_block_t block)
{
    struct littlefs_sb_info *sbi = (struct littlefs_sb_info *)c->context;
    uint32_t sector_size = sbi->sector_size ? sbi->sector_size : 512U;
    uint32_t sectors_per_block = c->block_size / sector_size;
    uint32_t lba = block * sectors_per_block;
    uint32_t count = c->block_size / sector_size;
    int ret;

    ret = block_erase(sbi->bdev, lba, count);
    return ret == 0 ? LFS_ERR_OK : LFS_ERR_IO;
}

static int littlefs_bd_sync(const struct lfs_config *c)
{
    (void)c;
    return LFS_ERR_OK;
}

static int littlefs_bd_lock(const struct lfs_config *c)
{
    struct littlefs_sb_info *sbi = (struct littlefs_sb_info *)c->context;
    mutex_lock(&sbi->lock);
    return LFS_ERR_OK;
}

static int littlefs_bd_unlock(const struct lfs_config *c)
{
    struct littlefs_sb_info *sbi = (struct littlefs_sb_info *)c->context;
    mutex_unlock(&sbi->lock);
    return LFS_ERR_OK;
}

/*
 * 文件操作 (file_operations)
 */

static ssize_t littlefs_read(struct file *file, char *buf, size_t count, loff_t *ppos)
{
    struct littlefs_file *lf = (struct littlefs_file *)file->private_data;
    lfs_soff_t cur_pos;
    lfs_ssize_t ret;

    if (lf == NULL || lf->sbi == NULL) {
        return -EBADF;
    }
    if (buf == NULL) {
        return -EINVAL;
    }
    if (count == 0) {
        return 0;
    }

    cur_pos = lfs_file_tell(&lf->sbi->lfs, &lf->file);
    if (cur_pos != *ppos) {
        lfs_soff_t seek_res = lfs_file_seek(&lf->sbi->lfs, &lf->file,
                                            (lfs_soff_t)*ppos, LFS_SEEK_SET);
        if (seek_res < 0) {
            return lfs_to_errno((int)seek_res);
        }
    }

    ret = lfs_file_read(&lf->sbi->lfs, &lf->file, buf, count);
    if (ret < 0) {
        return lfs_to_errno((int)ret);
    }

    *ppos += ret;
    return ret;
}

static ssize_t littlefs_write(struct file *file, const char *buf, size_t count, loff_t *ppos)
{
    struct littlefs_file *lf = (struct littlefs_file *)file->private_data;
    lfs_soff_t cur_pos;
    lfs_ssize_t ret;

    if (lf == NULL || lf->sbi == NULL) {
        return -EBADF;
    }
    if (buf == NULL) {
        return -EINVAL;
    }
    if (count == 0) {
        return 0;
    }

    cur_pos = lfs_file_tell(&lf->sbi->lfs, &lf->file);
    if (cur_pos != *ppos) {
        lfs_soff_t seek_res = lfs_file_seek(&lf->sbi->lfs, &lf->file,
                                            (lfs_soff_t)*ppos, LFS_SEEK_SET);
        if (seek_res < 0) {
            return lfs_to_errno((int)seek_res);
        }
    }

    ret = lfs_file_write(&lf->sbi->lfs, &lf->file, buf, count);
    if (ret < 0) {
        return lfs_to_errno((int)ret);
    }

    *ppos += ret;
    if (file->f_inode != NULL && *ppos > file->f_inode->i_size) {
        file->f_inode->i_size = *ppos;
    }
    return ret;
}

static loff_t littlefs_llseek(struct file *file, loff_t offset, int whence)
{
    struct littlefs_file *lf = (struct littlefs_file *)file->private_data;
    lfs_soff_t res;
    int lfs_whence;

    if (lf == NULL || lf->sbi == NULL) {
        return -EBADF;
    }

    switch (whence) {
    case SEEK_SET: lfs_whence = LFS_SEEK_SET; break;
    case SEEK_CUR: lfs_whence = LFS_SEEK_CUR; break;
    case SEEK_END: lfs_whence = LFS_SEEK_END; break;
    default:       return -EINVAL;
    }

    res = lfs_file_seek(&lf->sbi->lfs, &lf->file, (lfs_soff_t)offset, lfs_whence);
    if (res < 0) {
        return lfs_to_errno((int)res);
    }
    file->f_pos = (loff_t)res;
    return file->f_pos;
}

static int littlefs_file_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
    struct littlefs_file *lf;
    int res;

    (void)start;
    (void)end;
    (void)datasync;

    if (file == NULL || file->private_data == NULL) {
        return -EBADF;
    }
    lf = (struct littlefs_file *)file->private_data;

    res = lfs_file_sync(&lf->sbi->lfs, &lf->file);
    if (res < 0) {
        return lfs_to_errno(res);
    }
    if (lf->sbi->cfg.sync != NULL) {
        int sres = lf->sbi->cfg.sync(&lf->sbi->cfg);
        if (sres < 0) {
            return lfs_to_errno(sres);
        }
    }
    return 0;
}

static int littlefs_file_open(struct inode *inode, struct file *file)
{
    struct super_block *sb;
    struct littlefs_sb_info *sbi;
    struct littlefs_file *lf;
    char path[256];
    int lfs_flags = 0;
    int res;

    if (!file->f_path.dentry) {
        return -EINVAL;
    }
    if (vfs_dentry_get(file->f_path.dentry) != 0) {
        return -EINVAL;
    }

    if (get_littlefs_path(file->f_path.dentry, path, sizeof(path)) != 0) {
        vfs_dentry_put(file->f_path.dentry);
        return -ENAMETOOLONG;
    }

    sb = file->f_path.dentry->d_sb;
    if (sb == NULL || sb->s_fs_info == NULL) {
        vfs_dentry_put(file->f_path.dentry);
        return -EINVAL;
    }
    sbi = (struct littlefs_sb_info *)sb->s_fs_info;

    switch (file->f_flags & O_ACCMODE) {
    case O_RDONLY: lfs_flags = LFS_O_RDONLY; break;
    case O_WRONLY: lfs_flags = LFS_O_WRONLY; break;
    case O_RDWR:   lfs_flags = LFS_O_RDWR;   break;
    default:
        vfs_dentry_put(file->f_path.dentry);
        return -EINVAL;
    }

    if (file->f_flags & O_CREAT) {
        lfs_flags |= LFS_O_CREAT;
    }
    if (file->f_flags & O_EXCL) {
        lfs_flags |= LFS_O_EXCL;
    }
    if (file->f_flags & O_TRUNC) {
        lfs_flags |= LFS_O_TRUNC;
    }
    if (file->f_flags & O_APPEND) {
        lfs_flags |= LFS_O_APPEND;
    }

    lf = kzalloc(sizeof(*lf), GFP_KERNEL);
    if (lf == NULL) {
        vfs_dentry_put(file->f_path.dentry);
        return -ENOMEM;
    }
    lf->sbi = sbi;

    res = lfs_file_open(&sbi->lfs, &lf->file, path, lfs_flags);
    if (res < 0) {
        kfree(lf);
        vfs_dentry_put(file->f_path.dentry);
        return lfs_to_errno(res);
    }

    if (file->f_flags & O_TRUNC) {
        inode->i_size = 0;
    } else {
        lfs_soff_t sz = lfs_file_size(&sbi->lfs, &lf->file);
        if (sz >= 0) {
            inode->i_size = (loff_t)sz;
        }
    }

    if (file->f_flags & O_APPEND) {
        lfs_soff_t end_pos = lfs_file_seek(&sbi->lfs, &lf->file, 0, LFS_SEEK_END);
        if (end_pos >= 0) {
            file->f_pos = (loff_t)end_pos;
        }
    }

    file->private_data = lf;
    return 0;
}

static int littlefs_release(struct inode *inode, struct file *file)
{
    struct littlefs_file *lf = (struct littlefs_file *)file->private_data;
    int res = 0;
    (void)inode;

    if (lf != NULL) {
        res = lfs_file_close(&lf->sbi->lfs, &lf->file);
        kfree(lf);
        file->private_data = NULL;
        vfs_dentry_put(file->f_path.dentry);
    }
    return res < 0 ? lfs_to_errno(res) : 0;
}

static int littlefs_same_file(struct file *left, struct file *right)
{
    if (left == NULL || right == NULL) {
        return -EBADF;
    }
    if (left == right) {
        return 1;
    }
    return (left->f_inode != NULL && left->f_inode == right->f_inode) ? 1 : 0;
}

static struct file_operations littlefs_file_ops = {
    .read = littlefs_read,
    .write = littlefs_write,
    .open = littlefs_file_open,
    .release = littlefs_release,
    .llseek = littlefs_llseek,
    .fsync = littlefs_file_fsync,
    .same_file = littlefs_same_file,
};

/*
 * 目录操作 (dir_operations)
 */

static int littlefs_dir_open(struct inode *inode, struct file *file)
{
    struct super_block *sb;
    struct littlefs_sb_info *sbi;
    struct littlefs_dir *ld;
    char path[256];
    int res;
    (void)inode;

    if (!file->f_path.dentry) {
        return -EINVAL;
    }
    if (vfs_dentry_get(file->f_path.dentry) != 0) {
        return -EINVAL;
    }

    if (get_littlefs_path(file->f_path.dentry, path, sizeof(path)) != 0) {
        vfs_dentry_put(file->f_path.dentry);
        return -ENAMETOOLONG;
    }

    sb = file->f_path.dentry->d_sb;
    if (sb == NULL || sb->s_fs_info == NULL) {
        vfs_dentry_put(file->f_path.dentry);
        return -EINVAL;
    }
    sbi = (struct littlefs_sb_info *)sb->s_fs_info;

    ld = kzalloc(sizeof(*ld), GFP_KERNEL);
    if (ld == NULL) {
        vfs_dentry_put(file->f_path.dentry);
        return -ENOMEM;
    }
    ld->sbi = sbi;

    res = lfs_dir_open(&sbi->lfs, &ld->dir, path);
    if (res < 0) {
        kfree(ld);
        vfs_dentry_put(file->f_path.dentry);
        return lfs_to_errno(res);
    }

    file->private_data = ld;
    return 0;
}

static int littlefs_dir_release(struct inode *inode, struct file *file)
{
    struct littlefs_dir *ld = (struct littlefs_dir *)file->private_data;
    int res = 0;
    (void)inode;

    if (ld != NULL) {
        res = lfs_dir_close(&ld->sbi->lfs, &ld->dir);
        kfree(ld);
        file->private_data = NULL;
        vfs_dentry_put(file->f_path.dentry);
    }
    return res < 0 ? lfs_to_errno(res) : 0;
}

static int littlefs_readdir(struct file *file, void *dirent, filldir_t filldir)
{
    struct littlefs_dir *ld = (struct littlefs_dir *)file->private_data;
    struct lfs_info info;
    loff_t i;
    int res;

    if (ld == NULL || ld->sbi == NULL) {
        return -EBADF;
    }
    if (file->f_pos < 0) {
        return -EINVAL;
    }

    res = lfs_dir_rewind(&ld->sbi->lfs, &ld->dir);
    if (res < 0) {
        return lfs_to_errno(res);
    }

    for (i = 0; i < file->f_pos; i++) {
        res = lfs_dir_read(&ld->sbi->lfs, &ld->dir, &info);
        if (res < 0) {
            return lfs_to_errno(res);
        }
        if (res == 0) {
            return 0;
        }
    }

    for (;;) {
        res = lfs_dir_read(&ld->sbi->lfs, &ld->dir, &info);
        if (res < 0) {
            return lfs_to_errno(res);
        }
        if (res == 0) {
            return 0;
        }

        unsigned dtype = (info.type == LFS_TYPE_DIR) ? DT_DIR : DT_REG;
        if (filldir(dirent, info.name, strlen(info.name), file->f_pos, 0, dtype) != 0) {
            return 0;
        }
        file->f_pos++;
    }
}

static struct file_operations littlefs_dir_ops = {
    .open = littlefs_dir_open,
    .readdir = littlefs_readdir,
    .release = littlefs_dir_release,
};

/*
 * Inode 操作 (inode_operations)
 */

static struct dentry *littlefs_lookup(struct inode *dir, struct dentry *dentry)
{
    struct super_block *sb;
    struct littlefs_sb_info *sbi;
    struct lfs_info info;
    char path[256];
    int res;

    if (dir == NULL || dentry == NULL) {
        return NULL;
    }
    if (get_littlefs_path(dentry, path, sizeof(path)) != 0) {
        return NULL;
    }

    sb = dir->i_sb;
    if (sb == NULL || sb->s_fs_info == NULL) {
        return NULL;
    }
    sbi = (struct littlefs_sb_info *)sb->s_fs_info;

    res = lfs_stat(&sbi->lfs, path, &info);
    if (res == LFS_ERR_OK) {
        int mode = (info.type == LFS_TYPE_DIR) ? (S_IFDIR | 0755) : (S_IFREG | 0644);
        struct inode *inode = littlefs_make_inode(sb, mode);
        if (inode != NULL) {
            inode->i_size = (loff_t)info.size;
            dentry->d_inode = inode;
        }
    }
    return NULL;
}

static int littlefs_create(struct inode *dir, struct dentry *dentry, int mode)
{
    struct super_block *sb;
    struct littlefs_sb_info *sbi;
    struct inode *inode;
    lfs_file_t file;
    char path[256];
    int res;

    if (dir == NULL || dentry == NULL) {
        return -EINVAL;
    }
    if (get_littlefs_path(dentry, path, sizeof(path)) != 0) {
        return -ENAMETOOLONG;
    }

    sb = dir->i_sb;
    if (sb == NULL || sb->s_fs_info == NULL) {
        return -EINVAL;
    }
    sbi = (struct littlefs_sb_info *)sb->s_fs_info;

    res = lfs_file_open(&sbi->lfs, &file, path, LFS_O_CREAT | LFS_O_EXCL | LFS_O_WRONLY);
    if (res < 0) {
        return lfs_to_errno(res);
    }
    (void)lfs_file_close(&sbi->lfs, &file);

    inode = littlefs_make_inode(sb, mode);
    if (inode == NULL) {
        return -ENOMEM;
    }
    inode->i_size = 0;
    dentry->d_inode = inode;
    return 0;
}

static int littlefs_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
    struct super_block *sb;
    struct littlefs_sb_info *sbi;
    struct inode *inode;
    char path[256];
    int res;
    (void)mode;

    if (dir == NULL || dentry == NULL) {
        return -EINVAL;
    }
    if (get_littlefs_path(dentry, path, sizeof(path)) != 0) {
        return -ENAMETOOLONG;
    }

    sb = dir->i_sb;
    if (sb == NULL || sb->s_fs_info == NULL) {
        return -EINVAL;
    }
    sbi = (struct littlefs_sb_info *)sb->s_fs_info;

    res = lfs_mkdir(&sbi->lfs, path);
    if (res < 0) {
        return lfs_to_errno(res);
    }

    inode = littlefs_make_inode(sb, S_IFDIR | 0755);
    if (inode == NULL) {
        return -ENOMEM;
    }
    dentry->d_inode = inode;
    return 0;
}

static int littlefs_unlink(struct inode *dir, struct dentry *dentry)
{
    struct super_block *sb;
    struct littlefs_sb_info *sbi;
    char path[256];
    int res;
    (void)dir;

    if (dentry == NULL) {
        return -EINVAL;
    }
    if (get_littlefs_path(dentry, path, sizeof(path)) != 0) {
        return -ENAMETOOLONG;
    }

    sb = dentry->d_sb;
    if (sb == NULL || sb->s_fs_info == NULL) {
        return -EINVAL;
    }
    sbi = (struct littlefs_sb_info *)sb->s_fs_info;

    res = lfs_remove(&sbi->lfs, path);
    return res < 0 ? lfs_to_errno(res) : 0;
}

static int littlefs_rmdir(struct inode *dir, struct dentry *dentry)
{
    struct super_block *sb;
    struct littlefs_sb_info *sbi;
    char path[256];
    int res;
    (void)dir;

    if (dentry == NULL) {
        return -EINVAL;
    }
    if (get_littlefs_path(dentry, path, sizeof(path)) != 0) {
        return -ENAMETOOLONG;
    }

    sb = dentry->d_sb;
    if (sb == NULL || sb->s_fs_info == NULL) {
        return -EINVAL;
    }
    sbi = (struct littlefs_sb_info *)sb->s_fs_info;

    res = lfs_remove(&sbi->lfs, path);
    return res < 0 ? lfs_to_errno(res) : 0;
}

static int littlefs_rename(struct inode *old_dir, struct dentry *old_dentry,
                           struct inode *new_dir, struct dentry *new_dentry)
{
    struct super_block *sb;
    struct littlefs_sb_info *sbi;
    char *paths;
    char *old_path;
    char *new_path;
    int res;
    (void)old_dir;
    (void)new_dir;

    if (old_dentry == NULL || new_dentry == NULL) {
        return -EINVAL;
    }

    paths = kmalloc(512, GFP_KERNEL);
    if (paths == NULL) {
        return -ENOMEM;
    }
    old_path = paths;
    new_path = paths + 256;

    if (get_littlefs_path(old_dentry, old_path, 256) != 0 ||
        get_littlefs_path(new_dentry, new_path, 256) != 0) {
        kfree(paths);
        return -ENAMETOOLONG;
    }

    sb = old_dentry->d_sb;
    if (sb == NULL || sb->s_fs_info == NULL) {
        kfree(paths);
        return -EINVAL;
    }
    sbi = (struct littlefs_sb_info *)sb->s_fs_info;

    res = lfs_rename(&sbi->lfs, old_path, new_path);
    kfree(paths);
    if (res < 0) {
        return lfs_to_errno(res);
    }

    new_dentry->d_inode = old_dentry->d_inode;
    return 0;
}

static int littlefs_truncate(struct inode *inode, loff_t length)
{
    if (inode == NULL || length < 0) {
        return -EINVAL;
    }
    return -ENOTSUP;
}

static struct inode_operations littlefs_dir_inode_ops = {
    .lookup = littlefs_lookup,
    .create = littlefs_create,
    .mkdir = littlefs_mkdir,
    .rmdir = littlefs_rmdir,
    .unlink = littlefs_unlink,
    .rename = littlefs_rename,
};

static struct inode_operations littlefs_file_inode_ops = {
    .truncate = littlefs_truncate,
};

/*
 * 超级块操作 (super_operations)
 */

static int littlefs_sync_fs(struct super_block *sb, int wait)
{
    struct littlefs_sb_info *sbi;

    (void)wait;
    if (sb == NULL || sb->s_fs_info == NULL) {
        return -EINVAL;
    }
    sbi = (struct littlefs_sb_info *)sb->s_fs_info;
    if (sbi->cfg.sync != NULL) {
        int res = sbi->cfg.sync(&sbi->cfg);
        if (res < 0) {
            return lfs_to_errno(res);
        }
    }
    return 0;
}

static const struct super_operations littlefs_super_ops = {
    .sync_fs = littlefs_sync_fs,
};

/*
 * 文件系统挂载与注销
 */

static void littlefs_kill_sb(struct super_block *sb)
{
    struct littlefs_sb_info *sbi;

    if (sb == NULL) {
        return;
    }
    sbi = (struct littlefs_sb_info *)sb->s_fs_info;
    if (sbi != NULL) {
        (void)lfs_unmount(&sbi->lfs);
        kfree(sbi);
        sb->s_fs_info = NULL;
    }
}

static struct dentry *littlefs_mount(struct file_system_type *fs_type, int flags,
                                     const char *dev_name, void *data)
{
    const struct device *bdev;
    struct super_block *sb;
    struct littlefs_sb_info *sbi;
    struct inode *root_inode;
    struct dentry *root_dentry;
    uint32_t sector_size = 512;
    uint32_t sector_count = 0;
    int res;

    (void)flags;
    (void)data;

    if (dev_name == NULL || dev_name[0] == '\0') {
        pr_err("littlefs: device name required\n");
        return NULL;
    }

    bdev = device_get_binding(dev_name);
    if (bdev == NULL || !device_is_ready(bdev)) {
        pr_err("littlefs: device %s not found or not ready\n", dev_name);
        return NULL;
    }

    if (block_get_info(bdev, &sector_size, &sector_count) != 0 || sector_size == 0) {
        sector_size = 512;
    }

    sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
    if (sbi == NULL) {
        return NULL;
    }
    mutex_init(&sbi->lock);
    sbi->bdev = bdev;
    sbi->sector_size = sector_size;

    /* 配置 LittleFS 参数 */
    sbi->cfg.context = sbi;
    sbi->cfg.read = littlefs_bd_read;
    sbi->cfg.prog = littlefs_bd_prog;
    sbi->cfg.erase = littlefs_bd_erase;
    sbi->cfg.sync = littlefs_bd_sync;
    sbi->cfg.lock = littlefs_bd_lock;
    sbi->cfg.unlock = littlefs_bd_unlock;

    sbi->cfg.read_size = sector_size;
    sbi->cfg.prog_size = sector_size;
    sbi->cfg.block_size = 4096;
    if (sector_count > 0 && sector_size > 0) {
        uint32_t sectors_per_block = 4096 / sector_size;
        sbi->cfg.block_count = sector_count / (sectors_per_block ? sectors_per_block : 1);
    } else {
        sbi->cfg.block_count = 0;
    }
    sbi->cfg.cache_size = sector_size;
    sbi->cfg.lookahead_size = 32;
    sbi->cfg.block_cycles = 500;

    res = lfs_mount(&sbi->lfs, &sbi->cfg);
    if (res == LFS_ERR_CORRUPT) {
        pr_info("littlefs: No valid filesystem on %s, formatting...\n", dev_name);
        res = lfs_format(&sbi->lfs, &sbi->cfg);
        if (res == LFS_ERR_OK) {
            res = lfs_mount(&sbi->lfs, &sbi->cfg);
        }
    }
    if (res != LFS_ERR_OK) {
        pr_err("littlefs: mount failed: %d\n", res);
        kfree(sbi);
        return NULL;
    }

    sb = kzalloc(sizeof(*sb), GFP_KERNEL);
    if (sb == NULL) {
        (void)lfs_unmount(&sbi->lfs);
        kfree(sbi);
        return NULL;
    }

    sb->s_type = fs_type;
    sb->s_fs_info = sbi;
    sb->s_op = &littlefs_super_ops;

    root_inode = littlefs_make_inode(sb, S_IFDIR | 0755);
    if (root_inode == NULL) {
        (void)lfs_unmount(&sbi->lfs);
        kfree(sb);
        kfree(sbi);
        return NULL;
    }

    root_dentry = kzalloc(sizeof(*root_dentry), GFP_KERNEL);
    if (root_dentry == NULL) {
        (void)lfs_unmount(&sbi->lfs);
        kfree(root_inode);
        kfree(sb);
        kfree(sbi);
        return NULL;
    }

    root_dentry->d_inode = root_inode;
    root_dentry->d_sb = sb;
    root_dentry->d_refs = 1;
    sb->s_root = root_dentry;

    return root_dentry;
}

static struct file_system_type littlefs_fs_type = {
    .name = "littlefs",
    .mount = littlefs_mount,
    .kill_sb = littlefs_kill_sb,
};

/**
 * init_littlefs - 向 VFS 注册 LittleFS 文件系统驱动
 */
int init_littlefs(void)
{
    return register_filesystem(&littlefs_fs_type);
}
