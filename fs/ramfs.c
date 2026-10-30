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
#include <kernel/list.h>
#include <kernel/mm.h>
#include <string.h>

struct ramfs_node {
  union {
    void *data;               /* For file: raw data */
    struct list_head dirents; /* For dir: list of ramfs_dirent */
  };
  size_t size;
  unsigned int open_count;
  int unlinked;
};

struct ramfs_dirent {
  struct list_head list;
  char *name;
  struct inode *inode;
};

static const struct file_operations ramfs_file_ops;
static const struct file_operations ramfs_dir_ops;
static const struct inode_operations ramfs_dir_inode_ops;
static const struct inode_operations ramfs_file_inode_ops;
static void ramfs_free_inode(struct inode *inode);

static unsigned long g_ramfs_next_ino = 1;

static struct inode *ramfs_get_inode(struct super_block *sb, int mode,
                                     dev_t dev) {
  (void)dev;
  struct inode *inode = nos_vfs_alloc_inode(sb);
  if (inode) {
    inode->i_ino = __atomic_fetch_add(&g_ramfs_next_ino, 1UL, __ATOMIC_RELAXED);
    inode->i_mode = mode;
    inode->i_nlink = (mode & S_IFDIR) ? 2U : 1U;
    inode->i_size = 0;
    inode->i_atime = inode->i_mtime = inode->i_ctime = (struct timespec){0, 0};

    struct ramfs_node *node = kzalloc(sizeof(struct ramfs_node), GFP_KERNEL);
    if (!node) {
      nos_vfs_destroy_inode(inode);
      return NULL;
    }
    inode->i_private = node;

    if (mode & S_IFDIR) {
      inode->i_op = &ramfs_dir_inode_ops;
      inode->i_fop = &ramfs_dir_ops;
      INIT_LIST_HEAD(&node->dirents);
    } else if (mode & S_IFREG) {
      inode->i_op = &ramfs_file_inode_ops;
      inode->i_fop = &ramfs_file_ops;
    }
  }
  return inode;
}

/*
 * File Operations
 */
static int ramfs_open(struct inode *inode, struct file *file)
{
    struct ramfs_node *node = inode->i_private;

    (void)file;
    mutex_lock(&inode->i_mutex);
    if (node->unlinked) {
        mutex_unlock(&inode->i_mutex);
        return -ENOENT;
    }
    node->open_count++;
    mutex_unlock(&inode->i_mutex);
    return 0;
}

static int ramfs_release(struct inode *inode, struct file *file)
{
    struct ramfs_node *node = inode->i_private;
    int destroy;

    (void)file;
    mutex_lock(&inode->i_mutex);
    node->open_count--;
    destroy = node->unlinked && node->open_count == 0;
    mutex_unlock(&inode->i_mutex);
    if (destroy) {
        ramfs_free_inode(inode);
    }
    return 0;
}

static ssize_t ramfs_read(struct file *file, char *buf, size_t count,
                          loff_t *ppos)
{
    struct inode *inode = file->f_inode;
    struct ramfs_node *node = inode->i_private;
    size_t left;

    mutex_lock(&inode->i_mutex);
    if (*ppos >= inode->i_size) {
        mutex_unlock(&inode->i_mutex);
        return 0;
    }
    left = inode->i_size - *ppos;
    if (count > left) count = left;
    if (node && node->data) memcpy(buf, (char *)node->data + *ppos, count);
    *ppos += count;
    mutex_unlock(&inode->i_mutex);
    return count;
}

static ssize_t ramfs_write(struct file *file, const char *buf, size_t count,
                           loff_t *ppos)
{
    struct inode *inode = file->f_inode;
    struct ramfs_node *node = inode->i_private;
    size_t pos;
    size_t new_size;

    if (!node) return -EIO;
    if (count == 0) return 0;
    mutex_lock(&inode->i_mutex);
    if (!(file->f_flags & O_APPEND) && *ppos < 0) {
        mutex_unlock(&inode->i_mutex);
        return -EINVAL;
    }
    pos = (file->f_flags & O_APPEND) ? (size_t)inode->i_size : (size_t)*ppos;
    new_size = pos + count;
    if (new_size < pos || new_size > UINT32_MAX) {
        mutex_unlock(&inode->i_mutex);
        return -EFBIG;
    }
    if (new_size > node->size) {
        void *new_data = kmalloc(new_size, GFP_KERNEL);
        if (!new_data) {
            mutex_unlock(&inode->i_mutex);
            return -ENOMEM;
        }
        if (node->data != NULL) {
            memcpy(new_data, node->data, node->size);
            kfree(node->data);
        }
        memset((char *)new_data + node->size, 0, new_size - node->size);
        node->data = new_data;
        node->size = new_size;
    }
    if (pos > (size_t)inode->i_size) {
        memset((char *)node->data + inode->i_size, 0, pos - (size_t)inode->i_size);
    }
    memcpy((char *)node->data + pos, buf, count);
    if (new_size > inode->i_size) inode->i_size = new_size;
    *ppos = (loff_t)new_size;
    mutex_unlock(&inode->i_mutex);
    return count;
}

static int ramfs_readdir(struct file *file, void *dirent, filldir_t filldir)
{
    struct inode *inode = file->f_inode;
    struct ramfs_node *node = inode->i_private;

    if (!node || !(inode->i_mode & S_IFDIR)) return -ENOTDIR;
    for (;;) {
        struct ramfs_dirent *de;
        char *name = NULL;
        size_t len = 0;
        u64 ino = 0;
        unsigned dtype = DT_REG;
        loff_t i = 0;
        int rc;

        /* Copy the entry while locked so a slow terminal callback does not
         * hold the directory mutex and stall other users of the directory. */
        mutex_lock(&inode->i_mutex);
        list_for_each_entry(de, &node->dirents, list) {
            if (i++ == file->f_pos) {
                len = strlen(de->name);
                name = kmalloc(len + 1, GFP_KERNEL);
                if (name != NULL) {
                    memcpy(name, de->name, len + 1);
                    ino = de->inode->i_ino;
                    dtype = (de->inode->i_mode & S_IFDIR) ? DT_DIR : DT_REG;
                }
                break;
            }
        }
        mutex_unlock(&inode->i_mutex);
        if (name == NULL) {
            return i > file->f_pos ? -ENOMEM : 0;
        }
        rc = filldir(dirent, name, (int)len, file->f_pos, ino, dtype);
        kfree(name);
        if (rc != 0) return 0;
        file->f_pos++;
    }
}

static const struct file_operations ramfs_file_ops = {
    .open = ramfs_open,
    .release = ramfs_release,
    .read = ramfs_read,
    .write = ramfs_write,
};

static const struct file_operations ramfs_dir_ops = {
    .open = ramfs_open,
    .release = ramfs_release,
    .readdir = ramfs_readdir,
};

static void ramfs_free_inode(struct inode *inode)
{
    struct ramfs_node *node;

    if (inode == NULL) {
        return;
    }
    node = inode->i_private;
    if (node != NULL) {
        if (!(inode->i_mode & S_IFDIR) && node->data != NULL) {
            kfree(node->data);
        }
        kfree(node);
    }
    nos_vfs_destroy_inode(inode);
}

/* A removed inode stays alive until its last open file is released. */
static void ramfs_drop_file_link(struct inode *inode)
{
    struct ramfs_node *node = inode->i_private;
    int destroy;

    mutex_lock(&inode->i_mutex);
    node->unlinked = 1;
    inode->i_nlink = 0;
    destroy = node->open_count == 0;
    mutex_unlock(&inode->i_mutex);
    if (destroy) {
        ramfs_free_inode(inode);
    }
}

/*
 * Inode Operations
 */
static int ramfs_add_entry(struct inode *dir, struct dentry *dentry, int mode)
{
    struct ramfs_node *node = dir->i_private;
    struct ramfs_dirent *de, *existing;
    struct inode *inode;
    int rc;

    if (node == NULL || dentry == NULL || dentry->d_name.name == NULL) return -EINVAL;
    inode = ramfs_get_inode(dir->i_sb, mode, 0);
    if (inode == NULL) return -ENOMEM;
    de = kzalloc(sizeof(*de), GFP_KERNEL);
    if (de == NULL) goto no_memory;
    de->name = kzalloc(strlen(dentry->d_name.name) + 1, GFP_KERNEL);
    if (de->name == NULL) {
        kfree(de);
        goto no_memory;
    }
    strcpy(de->name, dentry->d_name.name);
    de->inode = inode;

    mutex_lock(&dir->i_mutex);
    if (node->unlinked) {
        rc = -ENOENT;
        goto reject_entry;
    }
    list_for_each_entry(existing, &node->dirents, list) {
        if (strcmp(existing->name, de->name) == 0) {
            rc = -EEXIST;
            goto reject_entry;
        }
    }
    list_add_tail(&de->list, &node->dirents);
    dentry->d_inode = inode;
    mutex_unlock(&dir->i_mutex);
    return 0;

reject_entry:
    mutex_unlock(&dir->i_mutex);
    kfree(de->name);
    kfree(de);
    kfree(inode->i_private);
    nos_vfs_destroy_inode(inode);
    return rc;

no_memory:
    kfree(inode->i_private);
    nos_vfs_destroy_inode(inode);
    return -ENOMEM;
}

static int ramfs_create(struct inode *dir, struct dentry *dentry, int mode)
{
    return ramfs_add_entry(dir, dentry, mode | S_IFREG);
}

static int ramfs_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
    return ramfs_add_entry(dir, dentry, mode | S_IFDIR);
}

static struct dentry *ramfs_lookup(struct inode *dir, struct dentry *dentry)
{
    struct ramfs_node *node = dir->i_private;
    struct ramfs_dirent *de;

    if (node == NULL || !(dir->i_mode & S_IFDIR)) return NULL;
    mutex_lock(&dir->i_mutex);
    list_for_each_entry(de, &node->dirents, list) {
        if (strcmp(de->name, dentry->d_name.name) == 0) {
            dentry->d_inode = de->inode;
            break;
        }
    }
    mutex_unlock(&dir->i_mutex);
    return NULL;
}

static int ramfs_unlink(struct inode *dir, struct dentry *dentry)
{
    struct ramfs_node *node = dir ? dir->i_private : NULL;
    struct ramfs_dirent *de;

    if (node == NULL || dentry == NULL || dentry->d_name.name == NULL) {
        return -EINVAL;
    }
    mutex_lock(&dir->i_mutex);
    list_for_each_entry(de, &node->dirents, list) {
        if (strcmp(de->name, dentry->d_name.name) == 0) {
            if (de->inode->i_mode & S_IFDIR) {
                mutex_unlock(&dir->i_mutex);
                return -EISDIR;
            }
            list_del(&de->list);
            mutex_unlock(&dir->i_mutex);
            kfree(de->name);
            ramfs_drop_file_link(de->inode);
            kfree(de);
            return 0;
        }
    }
    mutex_unlock(&dir->i_mutex);
    return -ENOENT;
}

static int ramfs_rmdir(struct inode *dir, struct dentry *dentry)
{
    struct ramfs_node *node = dir ? dir->i_private : NULL;
    struct ramfs_dirent *de;

    if (node == NULL || dentry == NULL || dentry->d_name.name == NULL) {
        return -EINVAL;
    }
    mutex_lock(&dir->i_mutex);
    list_for_each_entry(de, &node->dirents, list) {
        if (strcmp(de->name, dentry->d_name.name) == 0) {
            struct ramfs_node *sub;

            if (!(de->inode->i_mode & S_IFDIR)) {
                mutex_unlock(&dir->i_mutex);
                return -ENOTDIR;
            }
            sub = de->inode->i_private;
            if (sub != NULL && !list_empty(&sub->dirents)) {
                mutex_unlock(&dir->i_mutex);
                return -ENOTEMPTY;
            }
            list_del(&de->list);
            mutex_unlock(&dir->i_mutex);
            kfree(de->name);
            ramfs_drop_file_link(de->inode);
            kfree(de);
            return 0;
        }
    }
    mutex_unlock(&dir->i_mutex);
    return -ENOENT;
}

static int ramfs_rename(struct inode *old_dir, struct dentry *old_dentry,
                        struct inode *new_dir, struct dentry *new_dentry)
{
    struct ramfs_node *old_node = old_dir ? old_dir->i_private : NULL;
    struct ramfs_node *new_node = new_dir ? new_dir->i_private : NULL;
    struct ramfs_dirent *de, *src_de = NULL, *dst_de = NULL;
    struct inode *first, *second;
    char *new_name;
    int rc = 0;

    if (old_node == NULL || new_node == NULL ||
        old_dentry == NULL || new_dentry == NULL ||
        old_dentry->d_name.name == NULL || new_dentry->d_name.name == NULL) {
        return -EINVAL;
    }
    new_name = kzalloc(strlen(new_dentry->d_name.name) + 1, GFP_KERNEL);
    if (new_name == NULL) {
        return -ENOMEM;
    }
    strcpy(new_name, new_dentry->d_name.name);

    /* Opposite-direction renames must acquire directory locks in the same order. */
    first = (uintptr_t)old_dir <= (uintptr_t)new_dir ? old_dir : new_dir;
    second = first == old_dir ? new_dir : old_dir;
    mutex_lock(&first->i_mutex);
    if (second != first) mutex_lock(&second->i_mutex);

    if (old_node->unlinked || new_node->unlinked) {
        rc = -ENOENT;
        goto out;
    }
    list_for_each_entry(de, &old_node->dirents, list) {
        if (strcmp(de->name, old_dentry->d_name.name) == 0) {
            src_de = de;
            break;
        }
    }
    if (src_de == NULL) {
        rc = -ENOENT;
        goto out;
    }
    if (old_dir == new_dir && strcmp(old_dentry->d_name.name, new_name) == 0)
        goto out;

    list_for_each_entry(de, &new_node->dirents, list) {
        if (strcmp(de->name, new_name) == 0) {
            dst_de = de;
            break;
        }
    }
    if (dst_de != NULL) {
        if ((dst_de->inode->i_mode & S_IFDIR) != (src_de->inode->i_mode & S_IFDIR)) {
            rc = (dst_de->inode->i_mode & S_IFDIR) ? -EISDIR : -ENOTDIR;
            goto out;
        }
        if (dst_de->inode->i_mode & S_IFDIR) {
            struct ramfs_node *sub = dst_de->inode->i_private;
            if (sub != NULL && !list_empty(&sub->dirents)) {
                rc = -ENOTEMPTY;
                goto out;
            }
        }
        list_del(&dst_de->list);
        kfree(dst_de->name);
        ramfs_drop_file_link(dst_de->inode);
        kfree(dst_de);
    }

    list_del(&src_de->list);
    kfree(src_de->name);
    src_de->name = new_name;
    new_name = NULL;
    list_add_tail(&src_de->list, &new_node->dirents);
    new_dentry->d_inode = src_de->inode;

out:
    if (second != first) mutex_unlock(&second->i_mutex);
    mutex_unlock(&first->i_mutex);
    if (new_name != NULL) kfree(new_name);
    return rc;
}

static int ramfs_truncate(struct inode *inode, loff_t length)
{
    struct ramfs_node *node;
    size_t target;

    if (inode == NULL || length < 0) {
        return -EINVAL;
    }
    if (inode->i_mode & S_IFDIR) {
        return -EISDIR;
    }
    node = inode->i_private;
    if (node == NULL) {
        return -EIO;
    }
    target = (size_t)length;
    if ((loff_t)target != length || target > UINT32_MAX) {
        return -EFBIG;
    }
    mutex_lock(&inode->i_mutex);
    if (target == 0) {
        if (node->data != NULL) {
            kfree(node->data);
            node->data = NULL;
        }
        node->size = 0;
        inode->i_size = 0;
    } else if (target > node->size) {
        void *new_data = kmalloc(target, GFP_KERNEL);
        if (new_data == NULL) {
            mutex_unlock(&inode->i_mutex);
            return -ENOMEM;
        }
        if (node->data != NULL) {
            memcpy(new_data, node->data, (size_t)inode->i_size);
            kfree(node->data);
        }
        if (target > (size_t)inode->i_size) {
            memset((char *)new_data + inode->i_size, 0, target - (size_t)inode->i_size);
        }
        node->data = new_data;
        node->size = target;
        inode->i_size = target;
    } else {
        if (target > (size_t)inode->i_size && node->data != NULL) {
            memset((char *)node->data + inode->i_size, 0, target - (size_t)inode->i_size);
        }
        inode->i_size = target;
    }
    mutex_unlock(&inode->i_mutex);
    return 0;
}

static const struct inode_operations ramfs_dir_inode_ops = {
    .create = ramfs_create,
    .lookup = ramfs_lookup,
    .mkdir = ramfs_mkdir,
    .rmdir = ramfs_rmdir,
    .unlink = ramfs_unlink,
    .rename = ramfs_rename,
};

static const struct inode_operations ramfs_file_inode_ops = {
    .truncate = ramfs_truncate,
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
  if (!root)
    return -ENOMEM; // destroy inode

  root->d_inode = inode;
  root->d_sb = sb;
  sb->s_root = root;

  // Initialize root list (already done in get_inode if DIR)

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
    .kill_sb = NULL,
};

int init_ramfs(void) { return register_filesystem(&ramfs_fs_type); }
