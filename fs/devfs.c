/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * DevFS: 极简设备文件系统 (DevFS)
 */

#include <fs/fs.h>
#include <fs/vfs.h>
#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <kernel/printk.h>
#include <kernel/spinlock.h>
#include <string.h>

#define DEVFS_ROOT_INO 1
#define DEVFS_MAX_DEVICES 8

/* 字符设备查找表项 */
struct devfs_entry {
    const char *name;
    const struct file_operations *fops;
    void *priv;
};

static struct devfs_entry devfs_table[DEVFS_MAX_DEVICES];
static SPINLOCK(devfs_lock);

/*
 * 注册字符设备至 DevFS
 */
int devfs_register_chrdev(const char *name, const struct file_operations *fops, void *priv)
{
    int empty_slot = -1;

    if (name == NULL || fops == NULL) {
        return -EINVAL;
    }

    /* 规范化设备名称：去除前导 "/dev/" 或 "/" */
    if (strncmp(name, "/dev/", 5) == 0) {
        name += 5;
    } else if (name[0] == '/') {
        name += 1;
    }

    if (name[0] == '\0') {
        return -EINVAL;
    }

    spin_lock(&devfs_lock);

    /* 检查是否已注册，若已存在则更新配置（支持幂等更新） */
    for (int i = 0; i < DEVFS_MAX_DEVICES; i++) {
        if (devfs_table[i].name != NULL && strcmp(devfs_table[i].name, name) == 0) {
            devfs_table[i].fops = fops;
            devfs_table[i].priv = priv;
            spin_unlock(&devfs_lock);
            return 0;
        }
        if (devfs_table[i].name == NULL && empty_slot < 0) {
            empty_slot = i;
        }
    }

    if (empty_slot < 0) {
        spin_unlock(&devfs_lock);
        return -ENOMEM;
    }

    devfs_table[empty_slot].name = name;
    devfs_table[empty_slot].fops = fops;
    devfs_table[empty_slot].priv = priv;

    spin_unlock(&devfs_lock);
    return 0;
}

/*
 * 内置空操作 open/release
 */
static int devfs_noop_open(struct inode *inode, struct file *file)
{
    (void)inode;
    (void)file;
    return 0;
}

static int devfs_noop_release(struct inode *inode, struct file *file)
{
    (void)inode;
    (void)file;
    return 0;
}

/*
 * 内置 /dev/null 字符设备实现
 */
static ssize_t devfs_null_read(struct file *file, char *buf, size_t count, loff_t *ppos)
{
    (void)file;
    (void)buf;
    (void)count;
    (void)ppos;
    return 0;
}

static ssize_t devfs_null_write(struct file *file, const char *buf, size_t count, loff_t *ppos)
{
    (void)file;
    (void)buf;
    (void)ppos;
    return (ssize_t)count;
}

const struct file_operations devfs_null_fops = {
    .open = devfs_noop_open,
    .release = devfs_noop_release,
    .read = devfs_null_read,
    .write = devfs_null_write,
};

/*
 * 内置 /dev/zero 字符设备实现
 */
static ssize_t devfs_zero_read(struct file *file, char *buf, size_t count, loff_t *ppos)
{
    (void)file;
    (void)ppos;
    if (buf == NULL && count > 0) {
        return -EFAULT;
    }
    if (count > 0) {
        memset(buf, 0, count);
    }
    return (ssize_t)count;
}

static ssize_t devfs_zero_write(struct file *file, const char *buf, size_t count, loff_t *ppos)
{
    (void)file;
    (void)buf;
    (void)ppos;
    return (ssize_t)count;
}

const struct file_operations devfs_zero_fops = {
    .open = devfs_noop_open,
    .release = devfs_noop_release,
    .read = devfs_zero_read,
    .write = devfs_zero_write,
};

/*
 * DevFS 目录读取与设备遍历
 */
static int devfs_readdir(struct file *file, void *dirent, filldir_t filldir)
{
    struct inode *inode = file->f_inode;
    int i;

    if (inode == NULL || inode->i_ino != DEVFS_ROOT_INO) {
        return -ENOTDIR;
    }

    spin_lock(&devfs_lock);
    for (i = 0; i < DEVFS_MAX_DEVICES; i++) {
        if (devfs_table[i].name == NULL) {
            continue;
        }
        if (i >= file->f_pos) {
            const char *name = devfs_table[i].name;
            if (filldir(dirent, name, (int)strlen(name), i, (u64)(i + 2), DT_CHR)) {
                break;
            }
            file->f_pos++;
        }
    }
    spin_unlock(&devfs_lock);
    return 0;
}

static struct file_operations devfs_dir_ops = {
    .readdir = devfs_readdir,
};

/*
 * DevFS 节点查找
 */
static struct dentry *devfs_lookup(struct inode *dir, struct dentry *dentry)
{
    int i;

    if (dir == NULL || dentry == NULL || dentry->d_name.name == NULL) {
        return NULL;
    }

    if (dir->i_ino != DEVFS_ROOT_INO) {
        return NULL;
    }

    spin_lock(&devfs_lock);
    for (i = 0; i < DEVFS_MAX_DEVICES; i++) {
        if (devfs_table[i].name != NULL &&
            strcmp((const char *)dentry->d_name.name, devfs_table[i].name) == 0) {
            const struct file_operations *fops = devfs_table[i].fops;
            void *priv = devfs_table[i].priv;
            spin_unlock(&devfs_lock);

            struct inode *inode = nos_vfs_alloc_inode(dir->i_sb);
            if (!inode) {
                return NULL;
            }

            inode->i_ino = i + 2;
            inode->i_mode = S_IFCHR | 0666;
            inode->i_nlink = 1;
            inode->i_size = 0;
            inode->i_op = NULL;
            inode->i_fop = fops;
            inode->i_private = priv;
            dentry->d_inode = inode;
            return NULL;
        }
    }
    spin_unlock(&devfs_lock);

    return NULL;
}

static struct inode_operations devfs_dir_inode_ops = {
    .lookup = devfs_lookup,
};

/*
 * DevFS 超级块初始化与挂载
 */
static int devfs_fill_super(struct super_block *sb, void *data, int silent)
{
    struct inode *inode;
    struct dentry *root;

    (void)data;
    (void)silent;

    sb->s_blocksize = 1024;
    sb->s_blocksize_bits = 10;

    inode = nos_vfs_alloc_inode(sb);
    if (!inode) {
        return -ENOMEM;
    }

    inode->i_ino = DEVFS_ROOT_INO;
    inode->i_mode = S_IFDIR | 0755;
    inode->i_op = &devfs_dir_inode_ops;
    inode->i_fop = &devfs_dir_ops;

    root = kzalloc(sizeof(struct dentry), GFP_KERNEL);
    if (!root) {
        nos_vfs_destroy_inode(inode);
        return -ENOMEM;
    }

    root->d_inode = inode;
    root->d_sb = sb;
    sb->s_root = root;

    return 0;
}

static struct dentry *devfs_mount(struct file_system_type *fs_type, int flags,
                                  const char *dev_name, void *data)
{
    (void)fs_type;
    (void)dev_name;
    struct super_block *sb = kzalloc(sizeof(struct super_block), GFP_KERNEL);
    if (!sb) {
        return NULL;
    }

    if (devfs_fill_super(sb, data, flags)) {
        kfree(sb);
        return NULL;
    }
    return sb->s_root;
}

struct file_system_type devfs_fs_type = {
    .name = "devfs",
    .mount = devfs_mount,
};

/*
 * 初始化 DevFS：注册文件系统并挂载预置字符设备
 */
int init_devfs(void)
{
    int ret = register_filesystem(&devfs_fs_type);
    if (ret != 0) {
        return ret;
    }

    /* 注册内置默认字符设备 */
    devfs_register_chrdev("null", &devfs_null_fops, NULL);
    devfs_register_chrdev("zero", &devfs_zero_fops, NULL);
    return 0;
}
