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
#include <string.h>

static struct file_system_type *file_systems;
SPINLOCK(file_systems_lock);

/* Forward declaration */
struct file_system_type **find_filesystem(const char *name, unsigned len);

static struct dentry *root_dentry = NULL;
static struct super_block *root_sb;

/* Keep the initial public descriptor table deliberately small and static. */
#define VFS_MAX_OPEN_FILES 16

static struct file *open_files[VFS_MAX_OPEN_FILES];

struct dentry *vfs_get_root(void) { return root_dentry; }

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
  struct file_system_type **p = find_filesystem(fs_name, strlen(fs_name));
  type = *p;

  if (!type) {
      pr_info("VFS: vfs_mount fs type not found\r\n");
      return NULL;
  }
  
  pr_info("VFS: found type %s at %p, calling mount %p\r\n", type->name, type, type->mount);

  mnt_root = type->mount(type, flags, dev_name, data);
  if (!mnt_root)
    return NULL;

  // If it's the first mount, set as root
  if (!root_dentry) {
    root_dentry = mnt_root;
    root_sb = mnt_root->d_sb;
  }

  return mnt_root;
}

/*
 * Public API Wrappers
 */
static struct dentry *vfs_create_dentry(struct dentry *parent,
                                        const char *name, unsigned int len)
{
  struct dentry *dentry;

  dentry = kzalloc(sizeof(*dentry), GFP_KERNEL);
  if (!dentry)
    return NULL;

  dentry->d_parent = parent;
  dentry->d_sb = parent->d_sb;
  dentry->d_name.name = dentry->d_iname;
  dentry->d_name.len = len;
  memcpy(dentry->d_iname, name, len);
  dentry->d_iname[len] = '\0';
  return dentry;
}

static struct dentry *vfs_resolve_path(const char *path, int create)
{
  struct dentry *dentry;
  const char *name;

  if (!path || path[0] != '/' || !root_dentry)
    return NULL;

  dentry = root_dentry;
  name = path;
  while (*name == '/')
    name++;

  while (*name) {
    struct dentry *parent;
    struct qstr qstr;
    const char *next = name;
    unsigned int len;
    int last;

    while (*next && *next != '/')
      next++;
    len = next - name;
    if (len == 0 || len >= FS_NAME_LEN)
      return NULL;

    while (*next == '/')
      next++;
    last = *next == '\0';

    qstr.name = (const unsigned char *)name;
    qstr.len = len;
    qstr.hash = 0;
    parent = dentry;
    dentry = vfs_lookup(parent, &qstr);
    if (!dentry && create && last) {
      dentry = vfs_create_dentry(parent, name, len);
      if (!dentry || !dentry->d_parent->d_inode->i_op ||
          !dentry->d_parent->d_inode->i_op->create ||
          dentry->d_parent->d_inode->i_op->create(
              dentry->d_parent->d_inode, dentry, S_IFREG | 0644) != 0) {
        if (dentry)
          kfree(dentry);
        return NULL;
      }
    }
    if (!dentry || (!last && (!dentry->d_inode ||
                              !S_ISDIR(dentry->d_inode->i_mode))))
      return NULL;

    name = next;
  }

  return dentry;
}

int nos_open(const char *path, int flags, ...)
{
  struct dentry *dentry;
  struct file *file;
  int fd;
  int ret;

  dentry = vfs_resolve_path(path, flags & O_CREAT);
  if (!dentry)
    return -ENOENT;

  if (!dentry->d_inode || !dentry->d_inode->i_fop)
    return -EACCES;

  file = kzalloc(sizeof(*file), GFP_KERNEL);
  if (!file)
    return -ENOMEM;

  file->f_path.dentry = dentry;
  file->f_path.mnt = NULL;
  file->f_inode = dentry->d_inode;
  file->f_op = dentry->d_inode->i_fop;
  file->f_flags = flags;

  if (file->f_op->open) {
    ret = file->f_op->open(file->f_inode, file);
    if (ret) {
      kfree(file);
      return ret;
    }
  }

  for (fd = 0; fd < VFS_MAX_OPEN_FILES; fd++) {
    if (!open_files[fd]) {
      open_files[fd] = file;
      return fd;
    }
  }

  if (file->f_op->release)
    file->f_op->release(file->f_inode, file);
  kfree(file);
  return -EMFILE;
}

ssize_t nos_read(int fd, void *buf, size_t count)
{
  struct file *file;

  if (fd < 0 || fd >= VFS_MAX_OPEN_FILES || !buf)
    return -EBADF;

  file = open_files[fd];
  if (!file || !file->f_op || !file->f_op->read)
    return -EBADF;

  return file->f_op->read(file, buf, count, &file->f_pos);
}

ssize_t nos_write(int fd, const void *buf, size_t count)
{
  struct file *file;

  if (fd < 0 || fd >= VFS_MAX_OPEN_FILES || !buf)
    return -EBADF;

  file = open_files[fd];
  if (!file || !file->f_op || !file->f_op->write)
    return -EBADF;

  return file->f_op->write(file, buf, count, &file->f_pos);
}

int nos_close(int fd)
{
  struct file *file;
  int ret = 0;

  if (fd < 0 || fd >= VFS_MAX_OPEN_FILES || !open_files[fd])
    return -EBADF;

  file = open_files[fd];
  open_files[fd] = NULL;
  if (file->f_op && file->f_op->release)
    ret = file->f_op->release(file->f_inode, file);
  kfree(file);
  return ret;
}

/*
 * Simple Mount Point Table
 */
struct mount_point {
    struct inode *parent;
    char *name;
    struct dentry *root;
};

static struct mount_point mount_points[8];
static int mount_count = 0;

int vfs_bind_mount(struct dentry *target, struct dentry *mnt_root) {
    pr_info("vfs_bind: target=%s root=%p count=%d\r\n", target->d_name.name, mnt_root, mount_count);
    if (mount_count >= 8) {
        return -1; // Max mounts reached
    }
    
    // Store necessary info to intercept lookup
    mount_points[mount_count].parent = target->d_parent->d_inode;
    mount_points[mount_count].name = kstrdup(target->d_name.name, GFP_KERNEL); 
    mount_points[mount_count].root = mnt_root;
    
    mount_count++;
    return 0;
}

/*
 * Simple path lookup stub
 */
struct dentry *vfs_lookup(struct dentry *parent, struct qstr *name) {
  struct inode *dir = parent->d_inode;
  struct dentry *dentry;
  int i;
  
  if (!dir || !dir->i_op || !dir->i_op->lookup)
    return NULL;

  // Allocate dentry
  dentry = kzalloc(sizeof(struct dentry), GFP_KERNEL);
  if (!dentry)
    return NULL;
    
  dentry->d_parent = parent;
  dentry->d_sb = parent->d_sb;
  dentry->d_name.name = name->name; // Shallow copy? Should strdup?
  // Copy name logic roughly
  strncpy(dentry->d_iname, name->name, FS_NAME_LEN);
  dentry->d_iname[FS_NAME_LEN - 1] = 0;
  dentry->d_name.name = dentry->d_iname;
  dentry->d_name.len = name->len;

  // Call lookup
  if (dir->i_op->lookup(dir, dentry) == NULL) {
    // If d_inode is NULL, it's a negative dentry (not found)
    if (!dentry->d_inode) {
      kfree(dentry);
      return NULL;
    }
    
    // Check for mount points
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
