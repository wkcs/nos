/**
 * Copyright (C) 2024-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <fs/fs.h>
#include <fs/vfs.h>
#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <kernel/printk.h>

#define SYSFS_ROOT_INO 1

/*
 * File Operations
 */
static int sysfs_readdir(struct file *file, void *dirent, filldir_t filldir) {
    // Empty for now (except . and .. handled generally?)
    return 0;
}

static struct file_operations sysfs_dir_ops = {
    .readdir = sysfs_readdir,
};

/*
 * Inode Operations
 */
static struct dentry *sysfs_lookup(struct inode *dir, struct dentry *dentry) {
    return NULL;
}

static struct inode_operations sysfs_dir_inode_ops = {
    .lookup = sysfs_lookup,
};

/*
 * Superblock Operations
 */
static int sysfs_fill_super(struct super_block *sb, void *data, int silent) {
    struct inode *inode;
    struct dentry *root;

    // pr_info("sysfs: fill_super\r\n");

    sb->s_blocksize = 1024;
    sb->s_blocksize_bits = 10;

    inode = nos_vfs_alloc_inode(sb);
    if (!inode) return -ENOMEM;
    
    inode->i_ino = SYSFS_ROOT_INO;
    inode->i_mode = S_IFDIR | 0755;
    inode->i_op = &sysfs_dir_inode_ops;
    inode->i_fop = &sysfs_dir_ops;

    root = kzalloc(sizeof(struct dentry), GFP_KERNEL);
    if (!root) return -ENOMEM;

    root->d_inode = inode;
    root->d_sb = sb;
    sb->s_root = root;

    return 0;
}

static struct dentry *sysfs_mount(struct file_system_type *fs_type, int flags,
                                  const char *dev_name, void *data) {
    struct super_block *sb = kzalloc(sizeof(struct super_block), GFP_KERNEL);
    if (!sb) return NULL;
    
    if (sysfs_fill_super(sb, data, flags)) {
        kfree(sb);
        return NULL;
    }
    return sb->s_root;
}

static struct file_system_type sysfs_fs_type = {
    .name = "sysfs",
    .mount = sysfs_mount,
};

int init_sysfs(void) {
    return register_filesystem(&sysfs_fs_type);
}
