/**
 * Copyright (C) 2024-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_FS_VFS_H__
#define __NOS_FS_VFS_H__

#include <kernel/list.h>
#include <kernel/mutex.h>
#include <kernel/spinlock.h>

/* Types are defined in fs/fs.h */
#include <fs/fs.h>

typedef unsigned int size_t;
typedef int ssize_t;
typedef unsigned short umode_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef long time_t;
typedef unsigned int fmode_t;

struct timespec {
  time_t tv_sec;
  long tv_nsec;
};

struct kstatfs; /* Forward declaration */

#define FS_NAME_LEN 32

struct inode;
struct dentry;
struct file;
struct super_block;
struct file_system_type;

typedef int (*filldir_t)(void *, const char *, int, loff_t, u64, unsigned);

/*
 * Inode operations
 */
struct inode_operations {
  int (*create)(struct inode *, struct dentry *, int);
  struct dentry *(*lookup)(struct inode *, struct dentry *);
  int (*mkdir)(struct inode *, struct dentry *, int);
  int (*rmdir)(struct inode *, struct dentry *);
  int (*rename)(struct inode *, struct dentry *, struct inode *,
                struct dentry *);
};

/*
 * File operations
 */
struct file_operations {
  ssize_t (*read)(struct file *, char *, size_t, loff_t *);
  ssize_t (*write)(struct file *, const char *, size_t, loff_t *);
  int (*readdir)(struct file *, void *dirent, filldir_t filldir);
  int (*open)(struct inode *, struct file *);
  int (*release)(struct inode *, struct file *);
  loff_t (*llseek)(struct file *, loff_t, int);
};

/*
 * Inode definition
 */
struct inode {
  unsigned long i_ino;
  umode_t i_mode;
  unsigned int i_nlink;
  uid_t i_uid;
  gid_t i_gid;
  loff_t i_size;
  struct timespec i_atime;
  struct timespec i_mtime;
  struct timespec i_ctime;
  unsigned int i_blkbits;
  unsigned long i_blocks;
  unsigned short i_bytes;
  struct super_block *i_sb;
  const struct inode_operations *i_op;
  const struct file_operations *i_fop;
  struct hlist_node i_hash;
  struct list_head i_list;
  struct list_head i_sb_list;
  struct list_head i_dentry;
  unsigned long i_state;
  unsigned long i_flags;
  struct mutex i_mutex;
  void *i_private;
};

/*
 * Dentry operations
 */
struct dentry_operations {
  int (*d_revalidate)(struct dentry *, int);
  int (*d_hash)(struct dentry *, struct qstr *);
  int (*d_compare)(struct dentry *, struct qstr *, struct qstr *);
  int (*d_delete)(struct dentry *);
  void (*d_release)(struct dentry *);
  void (*d_iput)(struct dentry *, struct inode *);
};

/*
 * Dentry definition
 */
struct dentry {
  unsigned int d_flags;
  struct inode *d_inode;    /* Where the name belongs to - NULL is negative */
  struct hlist_node d_hash; /* lookup hash list */
  struct dentry *d_parent;  /* parent directory */
  struct qstr d_name;
  struct list_head d_lru;     /* LRU list */
  struct list_head d_child;   /* child of parent list */
  struct list_head d_subdirs; /* our children */
  struct list_head d_alias;   /* inode alias list */
  const struct dentry_operations *d_op;
  struct super_block *d_sb;  /* The root of the dentry tree */
  void *d_fsdata;            /* fs-specific data */
  char d_iname[FS_NAME_LEN]; /* small names */
};

/*
 * Super block operations
 */
struct super_operations {
  struct inode *(*alloc_inode)(struct super_block *sb);
  void (*destroy_inode)(struct inode *);
  void (*dirty_inode)(struct inode *);
  int (*write_inode)(struct inode *, int);
  void (*drop_inode)(struct inode *);
  void (*delete_inode)(struct inode *);
  void (*put_super)(struct super_block *);
  int (*sync_fs)(struct super_block *sb, int wait);
  int (*statfs)(struct dentry *, struct kstatfs *);
  int (*remount_fs)(struct super_block *, int *, char *);
  void (*clear_inode)(struct inode *);
  void (*umount_begin)(struct super_block *);
};

/*
 * Super block definition
 */
struct super_block {
  struct list_head s_list; /* Keep this first */
  dev_t s_dev;             /* search index; _not_ kdev_t */
  unsigned long s_blocksize;
  unsigned char s_blocksize_bits;
  unsigned char s_dirt;
  unsigned long long s_maxbytes; /* Max file size */
  struct file_system_type *s_type;
  const struct super_operations *s_op;
  struct dentry *s_root;
  struct list_head s_inodes; /* all inodes */
  struct list_head s_files;
  struct list_head s_mounts; /* list of mounts; _not_ for fs use */
  void *s_fs_info;           /* Filesystem private info */
  char s_id[32];             /* Informational name */
  void *s_priv;
};

/*
 * File definition
 */
struct file {
  struct path f_path;
  struct inode *f_inode; /* cached value */
  const struct file_operations *f_op;
  unsigned int f_flags;
  fmode_t f_mode;
  loff_t f_pos;
  void *private_data;
  struct list_head f_list;
};

struct file_system_type {
  const char *name;
  int fs_flags;
  struct dentry *(*mount)(struct file_system_type *, int, const char *, void *);
  void (*kill_sb)(struct super_block *);
  struct module *owner;
  struct file_system_type *next;
  struct list_head fs_supers;
};

/* VFS API Prototypes */
struct dentry *vfs_mount(const char *fs_name, int flags, const char *dev_name,
                         void *data);
int vfs_unmount(struct dentry *target);
struct inode *nos_vfs_alloc_inode(struct super_block *sb);
void nos_vfs_destroy_inode(struct inode *inode);
int register_filesystem(struct file_system_type *fs);
int unregister_filesystem(struct file_system_type *fs);
int vfs_init(void);
struct dentry *vfs_get_root(void);
struct dentry *vfs_lookup(struct dentry *parent, struct qstr *name);

#endif /* __NOS_FS_VFS_H__ */
