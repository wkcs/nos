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
#include <lib/vsprintf.h>
#include <string.h>
#include "vfs_fd.h"

static struct file_system_type *file_systems;
SPINLOCK(file_systems_lock);

/* Forward declaration */
struct file_system_type **find_filesystem(const char *name, unsigned len);

static struct dentry *root_dentry = NULL;
static struct super_block *root_sb;
static const char *root_fs_name = "rootfs";
static const char *root_dev_name = "rootfs";
static SPINLOCK(vfs_mount_lock);

#define VFS_MAX_MOUNT_POINTS 8

struct mount_point {
    struct inode *parent;
    char *name;
    char *mount_path;
    char *dev_name;
    const char *fs_name;
    struct dentry *target;
    struct dentry *root;
    struct inode *orig_inode;
    struct super_block *orig_sb;
};

static struct mount_point mount_points[VFS_MAX_MOUNT_POINTS];
static int mount_count = 0;

static void vfs_ensure_dentry_lists(struct dentry *dentry)
{
    if (dentry == NULL) {
        return;
    }
    if (dentry->d_subdirs.next == NULL || dentry->d_subdirs.prev == NULL) {
        INIT_LIST_HEAD(&dentry->d_subdirs);
    }
    if (dentry->d_child.next == NULL || dentry->d_child.prev == NULL) {
        INIT_LIST_HEAD(&dentry->d_child);
    }
}

struct dentry *vfs_alloc_dentry(struct dentry *parent, const char *name,
                                unsigned int len)
{
    struct dentry *dentry;

    if (len >= FS_NAME_LEN) {
        len = FS_NAME_LEN - 1U;
    }
    dentry = kzalloc(sizeof(*dentry), GFP_KERNEL);
    if (dentry == NULL) {
        return NULL;
    }
    dentry->d_parent = parent;
    dentry->d_sb = parent ? parent->d_sb : NULL;
    if (name != NULL && len > 0U) {
        memcpy(dentry->d_iname, name, len);
    }
    dentry->d_iname[len] = '\0';
    dentry->d_name.name = (const unsigned char *)dentry->d_iname;
    dentry->d_name.len = len;
    INIT_LIST_HEAD(&dentry->d_subdirs);
    INIT_LIST_HEAD(&dentry->d_child);
    return dentry;
}

static void vfs_free_dentry_Subtree(struct dentry *dentry)
{
    struct dentry *child, *tmp;

    if (dentry == NULL) {
        return;
    }
    vfs_ensure_dentry_lists(dentry);
    list_for_each_entry_safe(child, tmp, &dentry->d_subdirs, d_child) {
        list_del(&child->d_child);
        vfs_free_dentry_Subtree(child);
        kfree(child);
    }
}

static struct dentry *vfs_find_cached_child(struct dentry *parent,
                                            const char *name, unsigned int len)
{
    struct dentry *child;

    if (parent == NULL || name == NULL) {
        return NULL;
    }
    vfs_ensure_dentry_lists(parent);
    list_for_each_entry(child, &parent->d_subdirs, d_child) {
        if (child->d_name.len == len &&
            strncmp((const char *)child->d_name.name, name, len) == 0 &&
            child->d_inode != NULL) {
            return child;
        }
    }
    return NULL;
}

struct dentry *vfs_get_root(void)
{
    struct dentry *root;
    spin_lock(&vfs_mount_lock);
    root = root_dentry;
    spin_unlock(&vfs_mount_lock);
    return root;
}

extern int init_ramfs(void);

int vfs_init(void) {
  int ret;

  ret = init_ramfs();
  if (ret) {
    pr_err("VFS: Failed to register ramfs\r\n");
    return ret;
  }

#ifdef CONFIG_FATFS
  extern int init_fatfs(void);
  ret = init_fatfs();
  if (ret) {
    pr_err("VFS: Failed to register fatfs\r\n");
    return ret;
  }
#endif

  ret = init_procfs();
  if (ret) {
    pr_err("VFS: Failed to register procfs\r\n");
    return ret;
  }

  ret = init_sysfs();
  if (ret) {
    pr_err("VFS: Failed to register sysfs\r\n");
    return ret;
  }

  return 0;
}

/*
 * File System Registration
 */
/*
 * File System Registration
 */
int register_filesystem(struct file_system_type *fs) {
  struct file_system_type **p;

  if (!fs)
    return -EINVAL;
    
  pr_info("VFS: Registering %s at %p\r\n", fs->name, fs);

  if (fs->next)
    return -EBUSY;

  spin_lock(&file_systems_lock);
  p = find_filesystem(fs->name, strlen(fs->name));
  if (*p) {
    spin_unlock(&file_systems_lock);
    return -EBUSY;
  }
  fs->next = NULL;
  /* append to the end */
  p = &file_systems;
  while (*p)
    p = &(*p)->next;
  *p = fs;
  spin_unlock(&file_systems_lock);
  return 0;
}

// ...

struct file_system_type **find_filesystem(const char *name, unsigned len) {
  struct file_system_type **p;
  for (p = &file_systems; *p; p = &(*p)->next)
    if (strlen((*p)->name) == len && strncmp((*p)->name, name, len) == 0)
      break;
  return p;
}

/*
 * Inode Management
 */
struct inode *nos_vfs_alloc_inode(struct super_block *sb) {
  struct inode *inode;

  if (sb->s_op && sb->s_op->alloc_inode)
    inode = sb->s_op->alloc_inode(sb);
  else {
    inode = (struct inode *)kmalloc(sizeof(struct inode), GFP_KERNEL);
    if (inode) {
      // Basic initialization
      memset(inode, 0, sizeof(struct inode));
    }
  }

  if (!inode)
    return NULL;

  inode->i_sb = sb;
  INIT_LIST_HEAD(&inode->i_list);
  INIT_LIST_HEAD(&inode->i_sb_list);
  INIT_LIST_HEAD(&inode->i_dentry);
  mutex_init(&inode->i_mutex);

  return inode;
}

void nos_vfs_destroy_inode(struct inode *inode) {
  if (inode->i_sb->s_op->destroy_inode)
    inode->i_sb->s_op->destroy_inode(inode);
  else
    kfree(inode);
}

/*
 * Mount/Unmount
 */
struct dentry *vfs_mount(const char *fs_name, int flags, const char *dev_name,
                         void *data) {
  struct file_system_type *type;
  struct dentry *mnt_root;

  pr_info("VFS: vfs_mount lookup %s\r\n", fs_name);
  
  // Simple lookup for now
  struct file_system_type **p;
  spin_lock(&file_systems_lock);
  p = find_filesystem(fs_name, strlen(fs_name));
  type = *p;
  spin_unlock(&file_systems_lock);

  if (!type) {
      pr_info("VFS: vfs_mount fs type not found\r\n");
      return NULL;
  }
  
  pr_info("VFS: found type %s at %p, calling mount %p\r\n", type->name, type, type->mount);

  mnt_root = type->mount(type, flags, dev_name, data);
  if (!mnt_root)
    return NULL;

  if (mnt_root->d_sb != NULL) {
    mnt_root->d_sb->s_type = type;
    if (dev_name != NULL) {
      strncpy(mnt_root->d_sb->s_id, dev_name, sizeof(mnt_root->d_sb->s_id) - 1U);
      mnt_root->d_sb->s_id[sizeof(mnt_root->d_sb->s_id) - 1U] = '\0';
    }
  }
  vfs_ensure_dentry_lists(mnt_root);

  // If it's the first mount, set as root
  spin_lock(&vfs_mount_lock);
  if (!root_dentry) {
    root_dentry = mnt_root;
    root_sb = mnt_root->d_sb;
    root_fs_name = type->name;
    root_dev_name = (dev_name && dev_name[0]) ? dev_name : type->name;
  }
  spin_unlock(&vfs_mount_lock);

  return mnt_root;
}

static void vfs_build_dentry_path(struct dentry *dentry, char *buf, size_t size)
{
    struct dentry *root = vfs_get_root();
    char *p;

    if (buf == NULL || size == 0U) {
        return;
    }
    if (dentry == NULL || dentry == root) {
        strncpy(buf, "/", size - 1U);
        buf[size - 1U] = '\0';
        return;
    }
    p = buf + size - 1U;
    *p = '\0';
    while (dentry != NULL && dentry != root) {
        unsigned int len = dentry->d_name.len;
        if ((size_t)(p - buf) <= len + 1U) {
            break;
        }
        p -= len;
        memcpy(p, dentry->d_name.name, len);
        *--p = '/';
        dentry = dentry->d_parent;
    }
    if (*p == '\0') {
        *--p = '/';
    }
    memmove(buf, p, (size_t)((buf + size) - p));
}

struct dentry *vfs_lookup_path(struct dentry *base, const char *path)
{
    struct dentry *curr;
    const char *p;

    if (path == NULL || *path == '\0') {
        return NULL;
    }
    curr = (path[0] == '/' || base == NULL) ? vfs_get_root() : base;
    if (curr == NULL) {
        return NULL;
    }
    p = path;
    while (*p == '/') {
        p++;
    }
    while (*p != '\0') {
        const char *start = p;
        unsigned int len = 0U;
        struct qstr q;
        struct dentry *next;

        while (*p != '\0' && *p != '/') {
            p++;
            len++;
        }
        while (*p == '/') {
            p++;
        }
        if (len == 0U || (len == 1U && start[0] == '.')) {
            continue;
        }
        if (len == 2U && start[0] == '.' && start[1] == '.') {
            if (curr->d_parent != NULL) {
                curr = curr->d_parent;
            }
            continue;
        }
        if (len >= FS_NAME_LEN) {
            return NULL;
        }
        if (curr->d_inode == NULL || !S_ISDIR(curr->d_inode->i_mode)) {
            return NULL;
        }
        q.name = (const unsigned char *)start;
        q.len = len;
        q.hash = 0U;
        next = vfs_lookup(curr, &q);
        if (next == NULL) {
            return NULL;
        }
        curr = next;
    }
    return curr;
}

static int vfs_split_parent(struct dentry *base, const char *path,
                            struct dentry **parent_out, char *name_out,
                            size_t name_cap)
{
    const char *slash;
    const char *end;
    size_t len;

    if (path == NULL || parent_out == NULL || name_out == NULL || name_cap == 0U) {
        return -EINVAL;
    }
    end = path + strlen(path);
    while (end > path && *(end - 1) == '/') {
        end--;
    }
    if (end == path) {
        return -EINVAL;
    }
    slash = end;
    while (slash > path && *(slash - 1) != '/') {
        slash--;
    }
    len = (size_t)(end - slash);
    if (len == 0U || len >= name_cap || len >= FS_NAME_LEN) {
        return -ENAMETOOLONG;
    }
    if ((len == 1U && slash[0] == '.') ||
        (len == 2U && slash[0] == '.' && slash[1] == '.')) {
        return -EINVAL;
    }
    memcpy(name_out, slash, len);
    name_out[len] = '\0';

    if (slash == path) {
        *parent_out = (path[0] == '/' || base == NULL) ? vfs_get_root() : base;
    } else {
        size_t dir_len = (size_t)(slash - path);
        char *dir_path = kmalloc((u32)(dir_len + 1U), GFP_KERNEL);
        if (dir_path == NULL) {
            return -ENOMEM;
        }
        memcpy(dir_path, path, dir_len);
        dir_path[dir_len] = '\0';
        *parent_out = vfs_lookup_path(base, dir_path);
        kfree(dir_path);
    }
    if (*parent_out == NULL || (*parent_out)->d_inode == NULL) {
        return -ENOENT;
    }
    if (!S_ISDIR((*parent_out)->d_inode->i_mode)) {
        return -ENOTDIR;
    }
    return 0;
}

int vfs_create_file(struct dentry *dir, const char *name, int mode,
                    struct dentry **out)
{
    struct dentry *dentry;
    struct qstr q;
    unsigned int len;
    int rc;

    if (dir == NULL || dir->d_inode == NULL || name == NULL) {
        return -EINVAL;
    }
    if (!S_ISDIR(dir->d_inode->i_mode)) {
        return -ENOTDIR;
    }
    len = (unsigned int)strlen(name);
    if (len == 0U || len >= FS_NAME_LEN) {
        return -EINVAL;
    }
    q.name = (const unsigned char *)name;
    q.len = len;
    q.hash = 0U;
    dentry = vfs_lookup(dir, &q);
    if (dentry != NULL) {
        if (out != NULL) {
            *out = dentry;
        }
        return 0;
    }
    if (dir->d_inode->i_op == NULL || dir->d_inode->i_op->create == NULL) {
        return -EROFS;
    }
    dentry = vfs_alloc_dentry(dir, name, len);
    if (dentry == NULL) {
        return -ENOMEM;
    }
    rc = dir->d_inode->i_op->create(dir->d_inode, dentry, (mode & 0777) | S_IFREG);
    if (rc != 0 || dentry->d_inode == NULL) {
        kfree(dentry);
        return rc != 0 ? rc : -EIO;
    }
    spin_lock(&vfs_mount_lock);
    vfs_ensure_dentry_lists(dir);
    list_add_tail(&dentry->d_child, &dir->d_subdirs);
    spin_unlock(&vfs_mount_lock);
    if (out != NULL) {
        *out = dentry;
    }
    return 0;
}

int vfs_make_dir(struct dentry *dir, const char *name, int mode,
                 struct dentry **out)
{
    struct dentry *dentry;
    struct qstr q;
    unsigned int len;
    int rc;

    if (dir == NULL || dir->d_inode == NULL || name == NULL) {
        return -EINVAL;
    }
    if (!S_ISDIR(dir->d_inode->i_mode)) {
        return -ENOTDIR;
    }
    len = (unsigned int)strlen(name);
    if (len == 0U || len >= FS_NAME_LEN) {
        return -EINVAL;
    }
    q.name = (const unsigned char *)name;
    q.len = len;
    q.hash = 0U;
    dentry = vfs_lookup(dir, &q);
    if (dentry != NULL) {
        if (out != NULL) {
            *out = dentry;
        }
        return -EEXIST;
    }
    if (dir->d_inode->i_op == NULL || dir->d_inode->i_op->mkdir == NULL) {
        return -EROFS;
    }
    dentry = vfs_alloc_dentry(dir, name, len);
    if (dentry == NULL) {
        return -ENOMEM;
    }
    rc = dir->d_inode->i_op->mkdir(dir->d_inode, dentry, (mode & 0777) | S_IFDIR);
    if (rc != 0 || dentry->d_inode == NULL) {
        kfree(dentry);
        return rc != 0 ? rc : -EIO;
    }
    spin_lock(&vfs_mount_lock);
    vfs_ensure_dentry_lists(dir);
    list_add_tail(&dentry->d_child, &dir->d_subdirs);
    spin_unlock(&vfs_mount_lock);
    if (out != NULL) {
        *out = dentry;
    }
    return 0;
}

int vfs_remove_entry(struct dentry *dir, const char *name, bool is_dir)
{
    struct dentry *cached;
    struct dentry temp;
    unsigned int len;
    int i;
    int rc;

    if (dir == NULL || dir->d_inode == NULL || name == NULL) {
        return -EINVAL;
    }
    len = (unsigned int)strlen(name);
    if (len == 0U || len >= FS_NAME_LEN) {
        return -EINVAL;
    }

    spin_lock(&vfs_mount_lock);
    for (i = 0; i < mount_count; i++) {
        if (mount_points[i].parent == dir->d_inode &&
            strcmp(mount_points[i].name, name) == 0) {
            spin_unlock(&vfs_mount_lock);
            return -EBUSY;
        }
    }
    spin_unlock(&vfs_mount_lock);

    memset(&temp, 0, sizeof(temp));
    temp.d_parent = dir;
    temp.d_sb = dir->d_sb;
    memcpy(temp.d_iname, name, len);
    temp.d_iname[len] = '\0';
    temp.d_name.name = (const unsigned char *)temp.d_iname;
    temp.d_name.len = len;

    if (dir->d_inode->i_op == NULL) {
        return -EROFS;
    }
    if (is_dir) {
        if (dir->d_inode->i_op->rmdir == NULL) {
            return -EROFS;
        }
        rc = dir->d_inode->i_op->rmdir(dir->d_inode, &temp);
    } else {
        if (dir->d_inode->i_op->unlink == NULL) {
            return -EROFS;
        }
        rc = dir->d_inode->i_op->unlink(dir->d_inode, &temp);
    }
    if (rc == 0) {
        spin_lock(&vfs_mount_lock);
        cached = vfs_find_cached_child(dir, name, len);
        if (cached != NULL) {
            list_del(&cached->d_child);
            vfs_free_dentry_Subtree(cached);
            kfree(cached);
        }
        spin_unlock(&vfs_mount_lock);
    }
    return rc;
}

int vfs_rename_entry(struct dentry *old_dir, const char *old_name,
                     struct dentry *new_dir, const char *new_name)
{
    struct dentry old_temp, new_temp;
    struct dentry *cached_old, *cached_new;
    unsigned int old_len, new_len;
    int rc;

    if (old_dir == NULL || new_dir == NULL ||
        old_dir->d_inode == NULL || new_dir->d_inode == NULL ||
        old_name == NULL || new_name == NULL) {
        return -EINVAL;
    }
    if (old_dir->d_sb != new_dir->d_sb) {
        return -EXDEV;
    }
    old_len = (unsigned int)strlen(old_name);
    new_len = (unsigned int)strlen(new_name);
    if (old_len == 0U || old_len >= FS_NAME_LEN ||
        new_len == 0U || new_len >= FS_NAME_LEN) {
        return -ENAMETOOLONG;
    }
    if (old_dir == new_dir && strcmp(old_name, new_name) == 0) {
        struct qstr name = { .name = (const unsigned char *)old_name,
                             .len = old_len, .hash = 0 };
        return vfs_lookup(old_dir, &name) != NULL ? 0 : -ENOENT;
    }
    if (old_dir->d_inode->i_op == NULL ||
        old_dir->d_inode->i_op->rename == NULL) {
        return -EROFS;
    }

    memset(&old_temp, 0, sizeof(old_temp));
    old_temp.d_parent = old_dir;
    old_temp.d_sb = old_dir->d_sb;
    memcpy(old_temp.d_iname, old_name, old_len);
    old_temp.d_iname[old_len] = '\0';
    old_temp.d_name.name = (const unsigned char *)old_temp.d_iname;
    old_temp.d_name.len = old_len;

    memset(&new_temp, 0, sizeof(new_temp));
    new_temp.d_parent = new_dir;
    new_temp.d_sb = new_dir->d_sb;
    memcpy(new_temp.d_iname, new_name, new_len);
    new_temp.d_iname[new_len] = '\0';
    new_temp.d_name.name = (const unsigned char *)new_temp.d_iname;
    new_temp.d_name.len = new_len;

    rc = old_dir->d_inode->i_op->rename(old_dir->d_inode, &old_temp,
                                        new_dir->d_inode, &new_temp);
    if (rc == 0) {
        spin_lock(&vfs_mount_lock);
        cached_new = vfs_find_cached_child(new_dir, new_name, new_len);
        if (cached_new != NULL) {
            list_del(&cached_new->d_child);
            vfs_free_dentry_Subtree(cached_new);
            kfree(cached_new);
        }
        cached_old = vfs_find_cached_child(old_dir, old_name, old_len);
        if (cached_old != NULL) {
            list_del(&cached_old->d_child);
            cached_old->d_parent = new_dir;
            memcpy(cached_old->d_iname, new_name, new_len);
            cached_old->d_iname[new_len] = '\0';
            cached_old->d_name.name = (const unsigned char *)cached_old->d_iname;
            cached_old->d_name.len = new_len;
            vfs_ensure_dentry_lists(new_dir);
            list_add_tail(&cached_old->d_child, &new_dir->d_subdirs);
        }
        spin_unlock(&vfs_mount_lock);
    }
    return rc;
}

int vfs_truncate_dentry(struct dentry *dentry, loff_t length)
{
    if (dentry == NULL || dentry->d_inode == NULL || length < 0) {
        return -EINVAL;
    }
    if (S_ISDIR(dentry->d_inode->i_mode)) {
        return -EISDIR;
    }
    if (dentry->d_inode->i_op != NULL && dentry->d_inode->i_op->truncate != NULL) {
        return dentry->d_inode->i_op->truncate(dentry->d_inode, length);
    }
    return -EROFS;
}

static struct dentry *vfs_resolve_path(const char *path, int create)
{
    struct dentry *dentry;

    if (path == NULL || path[0] != '/') {
        return NULL;
    }
    dentry = vfs_lookup_path(vfs_get_root(), path);
    if (dentry == NULL && create) {
        struct dentry *parent = NULL;
        char name[FS_NAME_LEN];
        if (vfs_split_parent(vfs_get_root(), path, &parent, name, sizeof(name)) == 0) {
            (void)vfs_create_file(parent, name, S_IFREG | 0644, &dentry);
        }
    }
    return dentry;
}

int nos_open(const char *path, int flags, ...)
{
  struct dentry *dentry;
  struct file *file;
  int fd;
  int ret;

  if ((flags & O_TRUNC) && (flags & O_ACCMODE) == O_RDONLY)
    return -EINVAL;
  fd = vfs_fd_reserve();
  if (fd < 0)
    return fd;
  dentry = vfs_resolve_path(path, flags & O_CREAT);
  if (!dentry) {
    ret = -ENOENT;
    goto release_fd;
  }

  if (!dentry->d_inode || !dentry->d_inode->i_fop) {
    ret = -EACCES;
    goto release_fd;
  }

  file = kzalloc(sizeof(*file), GFP_KERNEL);
  if (!file) {
    ret = -ENOMEM;
    goto release_fd;
  }
  if ((flags & O_TRUNC) && !S_ISDIR(dentry->d_inode->i_mode)) {
    ret = vfs_truncate_dentry(dentry, 0);
    if (ret != 0 && !(ret == -ENOTSUP && dentry->d_sb &&
                      dentry->d_sb->s_type &&
                      strcmp(dentry->d_sb->s_type->name, "fatfs") == 0))
      goto free_file;
  }

  file->f_path.dentry = dentry;
  file->f_path.mnt = NULL;
  file->f_inode = dentry->d_inode;
  file->f_op = dentry->d_inode->i_fop;
  file->f_flags = flags;
  if (flags & O_APPEND) {
    file->f_pos = file->f_inode->i_size;
  }

  if (file->f_op->open) {
    ret = file->f_op->open(file->f_inode, file);
    if (ret) {
      goto free_file;
    }
  }

  vfs_fd_publish(fd, file);
  return fd;

free_file:
  kfree(file);
release_fd:
  vfs_fd_cancel(fd);
  return ret;
}

int nos_mkdir(const char *pathname, mode_t mode)
{
  struct dentry *parent = NULL;
  char name[FS_NAME_LEN];
  int rc = vfs_split_parent(vfs_get_root(), pathname, &parent, name, sizeof(name));
  if (rc != 0)
    return rc;
  return vfs_make_dir(parent, name, (int)mode, NULL);
}

int nos_rmdir(const char *pathname)
{
  struct dentry *parent = NULL;
  char name[FS_NAME_LEN];
  int rc = vfs_split_parent(vfs_get_root(), pathname, &parent, name, sizeof(name));
  if (rc != 0)
    return rc;
  return vfs_remove_entry(parent, name, true);
}

int nos_unlink(const char *pathname)
{
  struct dentry *parent = NULL;
  char name[FS_NAME_LEN];
  int rc = vfs_split_parent(vfs_get_root(), pathname, &parent, name, sizeof(name));
  if (rc != 0)
    return rc;
  return vfs_remove_entry(parent, name, false);
}

int nos_rename(const char *oldpath, const char *newpath)
{
  struct dentry *old_parent = NULL, *new_parent = NULL;
  char old_name[FS_NAME_LEN], new_name[FS_NAME_LEN];
  int rc;

  rc = vfs_split_parent(vfs_get_root(), oldpath, &old_parent, old_name, sizeof(old_name));
  if (rc != 0)
    return rc;
  rc = vfs_split_parent(vfs_get_root(), newpath, &new_parent, new_name, sizeof(new_name));
  if (rc != 0)
    return rc;
  return vfs_rename_entry(old_parent, old_name, new_parent, new_name);
}

int nos_truncate(const char *pathname, loff_t length)
{
  struct dentry *dentry = vfs_lookup_path(vfs_get_root(), pathname);
  if (!dentry)
    return -ENOENT;
  return vfs_truncate_dentry(dentry, length);
}

int nos_close(int fd)
{
  return vfs_fd_close(fd);
}

int vfs_bind_mount(struct dentry *target, struct dentry *mnt_root)
{
    char *name;
    char *mpath;
    char *dev;
    char path_buf[128];
    const char *fs_name = "unknown";
    const char *dev_str = "none";
    int i;

    if (target == NULL || target->d_parent == NULL ||
        target->d_parent->d_inode == NULL || target->d_name.name == NULL ||
        mnt_root == NULL) {
        return -EINVAL;
    }
    if (mnt_root->d_sb != NULL) {
        if (mnt_root->d_sb->s_type != NULL && mnt_root->d_sb->s_type->name != NULL) {
            fs_name = mnt_root->d_sb->s_type->name;
        }
        if (mnt_root->d_sb->s_id[0] != '\0') {
            dev_str = mnt_root->d_sb->s_id;
        } else {
            dev_str = fs_name;
        }
    }
    vfs_build_dentry_path(target, path_buf, sizeof(path_buf));

    spin_lock(&vfs_mount_lock);
    for (i = 0; i < mount_count; i++) {
        if (mount_points[i].parent == target->d_parent->d_inode &&
            strcmp(mount_points[i].name, (const char *)target->d_name.name) == 0) {
            mount_points[i].root = mnt_root;
            mount_points[i].fs_name = fs_name;
            target->d_inode = mnt_root->d_inode;
            target->d_sb = mnt_root->d_sb;
            vfs_free_dentry_Subtree(target);
            spin_unlock(&vfs_mount_lock);
            return 0;
        }
    }
    spin_unlock(&vfs_mount_lock);

    name = kstrdup((const char *)target->d_name.name, GFP_KERNEL);
    mpath = kstrdup(path_buf, GFP_KERNEL);
    dev = kstrdup(dev_str, GFP_KERNEL);
    if (name == NULL || mpath == NULL || dev == NULL) {
        if (name) kfree(name);
        if (mpath) kfree(mpath);
        if (dev) kfree(dev);
        return -ENOMEM;
    }
    spin_lock(&vfs_mount_lock);
    if (mount_count >= VFS_MAX_MOUNT_POINTS) {
        spin_unlock(&vfs_mount_lock);
        kfree(name);
        kfree(mpath);
        kfree(dev);
        return -ENOSPC;
    }
    mount_points[mount_count].parent = target->d_parent->d_inode;
    mount_points[mount_count].name = name;
    mount_points[mount_count].mount_path = mpath;
    mount_points[mount_count].dev_name = dev;
    mount_points[mount_count].fs_name = fs_name;
    mount_points[mount_count].target = target;
    mount_points[mount_count].root = mnt_root;
    mount_points[mount_count].orig_inode = target->d_inode;
    mount_points[mount_count].orig_sb = target->d_sb;
    target->d_inode = mnt_root->d_inode;
    target->d_sb = mnt_root->d_sb;
    vfs_free_dentry_Subtree(target);
    mount_count++;
    spin_unlock(&vfs_mount_lock);
    return 0;
}

int vfs_unmount(struct dentry *target)
{
    int i, j;

    if (target == NULL || target == root_dentry) {
        return -EBUSY;
    }
    spin_lock(&vfs_mount_lock);
    for (i = 0; i < mount_count; i++) {
        if (mount_points[i].target == target ||
            (target->d_parent &&
             mount_points[i].parent == target->d_parent->d_inode &&
             strcmp(mount_points[i].name, (const char *)target->d_name.name) == 0)) {
            char *name = mount_points[i].name;
            char *mpath = mount_points[i].mount_path;
            char *dev = mount_points[i].dev_name;

            if (mount_points[i].orig_inode != NULL) {
                target->d_inode = mount_points[i].orig_inode;
                target->d_sb = mount_points[i].orig_sb;
            }
            vfs_free_dentry_Subtree(target);
            for (j = i; j + 1 < mount_count; j++) {
                mount_points[j] = mount_points[j + 1];
            }
            mount_count--;
            spin_unlock(&vfs_mount_lock);
            if (name) kfree(name);
            if (mpath) kfree(mpath);
            if (dev) kfree(dev);
            return 0;
        }
    }
    spin_unlock(&vfs_mount_lock);
    return -EINVAL;
}

int vfs_get_mount_count(void)
{
    int count;
    spin_lock(&vfs_mount_lock);
    count = (root_dentry != NULL ? 1 : 0) + mount_count;
    spin_unlock(&vfs_mount_lock);
    return count;
}

int vfs_get_mount_entry(int index, struct vfs_mount_entry *entry)
{
    if (entry == NULL || index < 0) {
        return -EINVAL;
    }
    spin_lock(&vfs_mount_lock);
    if (root_dentry == NULL) {
        spin_unlock(&vfs_mount_lock);
        return -ENOENT;
    }
    if (index == 0) {
        entry->dev_name = root_dev_name;
        entry->mount_path = "/";
        entry->fs_name = root_fs_name;
        entry->dentry = root_dentry;
        spin_unlock(&vfs_mount_lock);
        return 0;
    }
    index -= 1;
    if (index >= mount_count) {
        spin_unlock(&vfs_mount_lock);
        return -ENOENT;
    }
    entry->dev_name = mount_points[index].dev_name;
    entry->mount_path = mount_points[index].mount_path;
    entry->fs_name = mount_points[index].fs_name;
    entry->dentry = mount_points[index].target;
    spin_unlock(&vfs_mount_lock);
    return 0;
}

int vfs_list_filesystems(char *buf, size_t size)
{
    struct file_system_type *fs;
    int pos = 0;

    if (buf == NULL || size == 0U) {
        return -EINVAL;
    }
    buf[0] = '\0';
    spin_lock(&file_systems_lock);
    for (fs = file_systems; fs != NULL; fs = fs->next) {
        bool nodev = strcmp(fs->name, "fatfs") != 0;
        int n = snprintf(buf + pos, size - (size_t)pos, "%s\t%s\n",
                         nodev ? "nodev" : "     ", fs->name);
        if (n < 0 || (size_t)(pos + n) >= size) {
            pos = (int)size - 1;
            break;
        }
        pos += n;
    }
    spin_unlock(&file_systems_lock);
    return pos;
}

void vfs_setup_standard_mounts(void)
{
    struct dentry *root = vfs_get_root();
    struct dentry *dir = NULL;
    struct dentry *mnt_root;

    if (root == NULL) {
        return;
    }
    (void)vfs_make_dir(root, "proc", 0755, &dir);
    if (dir == NULL) {
        dir = vfs_lookup_path(root, "/proc");
    }
    if (dir != NULL) {
        mnt_root = vfs_mount("procfs", 0, "proc", NULL);
        if (mnt_root != NULL) {
            (void)vfs_bind_mount(dir, mnt_root);
        }
    }

    dir = NULL;
    (void)vfs_make_dir(root, "sys", 0755, &dir);
    if (dir == NULL) {
        dir = vfs_lookup_path(root, "/sys");
    }
    if (dir != NULL) {
        mnt_root = vfs_mount("sysfs", 0, "sysfs", NULL);
        if (mnt_root != NULL) {
            (void)vfs_bind_mount(dir, mnt_root);
        }
    }

    (void)vfs_make_dir(root, "tmp", 0777, NULL);
}

/*
 * Simple path lookup stub
 */
struct dentry *vfs_lookup(struct dentry *parent, struct qstr *name) {
  struct inode *dir;
  struct dentry *dentry, *cached;
  int i;
  
  if (!parent || !name || !name->name)
    return NULL;

  spin_lock(&vfs_mount_lock);
  cached = vfs_find_cached_child(parent, (const char *)name->name, name->len);
  spin_unlock(&vfs_mount_lock);
  if (cached != NULL) {
    return cached;
  }

  dir = parent->d_inode;
  if (!dir || !dir->i_op || !dir->i_op->lookup)
    return NULL;

  // Allocate dentry
  dentry = vfs_alloc_dentry(parent, (const char *)name->name, name->len);
  if (!dentry)
    return NULL;

  // Call lookup
  if (dir->i_op->lookup(dir, dentry) == NULL) {
    // If d_inode is NULL, it's a negative dentry (not found)
    if (!dentry->d_inode) {
      kfree(dentry);
      return NULL;
    }
    
    // Check for mount points
    spin_lock(&vfs_mount_lock);
    for (i = 0; i < mount_count; i++) {
        if (mount_points[i].parent == dir) {
             if (strcmp(dentry->d_name.name, mount_points[i].name) == 0) {
            
                // Intercept! Replace inode with mount root inode
                dentry->d_inode = mount_points[i].root->d_inode;
                dentry->d_sb = mount_points[i].root->d_sb;
                // Also might need to update d_op if FS relies on it?
                // For now only inode matters for file ops.
                break;
             }
        }
    }
    cached = vfs_find_cached_child(parent, dentry->d_iname, dentry->d_name.len);
    if (cached != NULL) {
      spin_unlock(&vfs_mount_lock);
      kfree(dentry);
      return cached;
    }
    vfs_ensure_dentry_lists(parent);
    list_add_tail(&dentry->d_child, &parent->d_subdirs);
    spin_unlock(&vfs_mount_lock);

    return dentry;
  }

  kfree(dentry);
  return NULL;
}

void vfs_test(void) {
  struct dentry *root;
  struct inode *root_inode;
  struct file *f;
  char buf[32];
  int ret;

  pr_info("VFS: Starting verification...\r\n");

  /* 1. Initialize VFS (and RamFS registration) */
  // Assuming init_ramfs() is called externally or we call it here
  extern int init_ramfs(void);
  ret = init_ramfs();
  if (ret) {
    pr_info("VFS: init_ramfs failed: %d\r\n", ret);
    return;
  }

  /* 2. Mount RamFS */
  root = vfs_mount("ramfs", 0, NULL, NULL);
  if (!root) {
    pr_info("VFS: vfs_mount failed\r\n");
    return;
  }
  root_inode = root->d_inode;
  pr_info("VFS: Mounted ramfs at root. Inode: %p\r\n", root_inode);

  /* 3. Create a file (simulate open O_CREAT) */
  // For now, we manually create inode/dentry as we don't have full path lookup
  struct dentry *file_dentry;
  struct inode *file_inode;

  file_dentry = (struct dentry *)kmalloc(sizeof(struct dentry), GFP_KERNEL);
  if (!file_dentry)
    return;

  // Simulate lookup/create in root
  ret = root_inode->i_op->create(root_inode, file_dentry, S_IFREG | 0644);
  if (ret) {
    pr_info("VFS: Create file failed: %d\r\n", ret);
    return;
  }
  file_inode = file_dentry->d_inode;
  pr_info("VFS: Created file inode: %p\r\n", file_inode);

  /* 4. Write to file */
  // Create a file object
  f = (struct file *)kmalloc(sizeof(struct file), GFP_KERNEL);
  if (!f)
    return;
  f->f_inode = file_inode;
  f->f_pos = 0;

  char *msg = "Hello VFS from NOS!";
  ret = file_inode->i_fop->write(f, msg, strlen(msg), &f->f_pos);
  pr_info("VFS: Wrote %d bytes: %s\r\n", ret, msg);

  /* 5. Read from file */
  f->f_pos = 0; // Reset pos
  memset(buf, 0, sizeof(buf));
  ret = file_inode->i_fop->read(f, buf, sizeof(buf), &f->f_pos);
  pr_info("VFS: Read %d bytes: %s\r\n", ret, buf);

  if (strcmp(msg, buf) == 0) {
    pr_info("VFS: Verification SUCCEEDED!\r\n");
  } else {
    pr_info("VFS: Verification FAILED!\r\n");
  }

  kfree(f);
  // Cleanup skipped for test
}
