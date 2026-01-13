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
#include <string.h>

struct ramfs_node {
  void *data;
  size_t size;
};

static const struct file_operations ramfs_file_ops;
static const struct inode_operations ramfs_dir_inode_ops;
static const struct inode_operations ramfs_file_inode_ops;

static struct inode *ramfs_get_inode(struct super_block *sb, int mode,
                                     dev_t dev) {
  struct inode *inode = nos_vfs_alloc_inode(sb);
  if (inode) {
    inode->i_mode = mode;
    inode->i_size = 0;
    inode->i_atime = inode->i_mtime = inode->i_ctime = (struct timespec){0, 0};

    if (mode & S_IFDIR) {
      inode->i_op = &ramfs_dir_inode_ops;
      inode->i_fop = &ramfs_file_ops; // Directory ops if needed
    } else if (mode & S_IFREG) {
      inode->i_op = &ramfs_file_inode_ops;
      inode->i_fop = &ramfs_file_ops;
      inode->i_private = kzalloc(sizeof(struct ramfs_node), GFP_KERNEL);
    }
  }
  return inode;
}

/*
 * File Operations
 */
static ssize_t ramfs_read(struct file *file, char *buf, size_t count,
                          loff_t *ppos) {
  struct inode *inode = file->f_inode;
  struct ramfs_node *node = inode->i_private;
  size_t left;

  if (*ppos >= inode->i_size)
    return 0;

  left = inode->i_size - *ppos;
  if (count > left)
    count = left;

  if (node && node->data)
    memcpy(buf, (char *)node->data + *ppos, count);

  *ppos += count;
  return count;
}

static ssize_t ramfs_write(struct file *file, const char *buf, size_t count,
                           loff_t *ppos) {
  struct inode *inode = file->f_inode;
  struct ramfs_node *node = inode->i_private;
  size_t pos = *ppos;
  size_t new_size = pos + count;

  if (!node)
    return -EIO;

  if (new_size > node->size) {
    void *new_data = krealloc(node->data, new_size, GFP_KERNEL);
    if (!new_data)
      return -ENOMEM;
    node->data = new_data;
    node->size = new_size;
  }

  memcpy((char *)node->data + pos, buf, count);
  if (new_size > inode->i_size)
    inode->i_size = new_size;

  *ppos += count;
  return count;
}

static const struct file_operations ramfs_file_ops = {
    .read = ramfs_read,
    .write = ramfs_write,
};

/*
 * Inode Operations
 */
static int ramfs_create(struct inode *dir, struct dentry *dentry, int mode) {
  struct inode *inode = ramfs_get_inode(dir->i_sb, mode | S_IFREG, 0);
  if (!inode)
    return -ENOMEM;

  dentry->d_inode = inode;
  return 0;
}

static int ramfs_mkdir(struct inode *dir, struct dentry *dentry, int mode) {
  struct inode *inode = ramfs_get_inode(dir->i_sb, mode | S_IFDIR, 0);
  if (!inode)
    return -ENOMEM;

  dentry->d_inode = inode;
  // Increase link count for parent?
  return 0;
}

static const struct inode_operations ramfs_dir_inode_ops = {
    .create = ramfs_create,
    // .lookup = simple_lookup,
    .mkdir = ramfs_mkdir,
};

/*
 * Superblock Operations
 */
static const struct super_operations ramfs_ops = {
    .statfs = NULL,
};

static int ramfs_fill_super(struct super_block *sb, void *data, int silent) {
  struct inode *inode;
  struct dentry *root;

  sb->s_blocksize = 1024;
  sb->s_blocksize_bits = 10;
  sb->s_op = &ramfs_ops;

  inode = ramfs_get_inode(sb, S_IFDIR | 0755, 0);
  if (!inode)
    return -ENOMEM;

  root = (struct dentry *)kzalloc(sizeof(struct dentry), GFP_KERNEL);
  if (!root) {
    // destroy inode
    return -ENOMEM;
  }
  root->d_inode = inode;
  root->d_sb = sb;
  sb->s_root = root;

  return 0;
}

static struct dentry *ramfs_mount(struct file_system_type *fs_type, int flags,
                                  const char *dev_name, void *data) {
  struct super_block *sb = kzalloc(sizeof(struct super_block), GFP_KERNEL);
  if (!sb)
    return NULL;

  if (ramfs_fill_super(sb, data, flags)) {
    kfree(sb);
    return NULL;
  }

  return sb->s_root;
}

static struct file_system_type ramfs_fs_type = {
    .name = "ramfs",
    .mount = ramfs_mount,
    .kill_sb = NULL, // should implement kill_sb to free memory
};

int init_ramfs(void) { return register_filesystem(&ramfs_fs_type); }
