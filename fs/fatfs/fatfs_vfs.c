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

#include "ff.h"

static struct inode_operations fatfs_dir_inode_ops;
static struct inode_operations fatfs_file_inode_ops;
static struct file_operations fatfs_dir_ops;
static struct file_operations fatfs_file_ops;

/* Helper to reconstruct full path from dentry for FatFs */
// Note: This is expensive but necessary for FatFs which requires full string paths.
// Ideally processing should happen on dentry tree, but FatFs manages the disk structure.
// We assume mount point is root or handles prefix correctly. For "0:/", we need to prepend drive.
static int get_fatfs_path(struct dentry *dentry, char *buf, int size) {
  char *p = buf + size - 1;
  struct dentry *root = dentry->d_sb->s_root;
  *p = '\0';

  if (dentry == root) {
    strcpy(buf, "0:/");
    return 0;
  }

  while (dentry && dentry != root) {
    int len = dentry->d_name.len;
    p -= len;
    if (p <= buf)
      return -ENAMETOOLONG;
    memcpy(p, dentry->d_name.name, len);
    *--p = '/';
    dentry = dentry->d_parent;
  }
  
  // Prepend drive "0:"
  if (p - 2 <= buf)
      return -ENAMETOOLONG; // optimize check
      
  // Shift to make room for "0:"
  // or just copy result to new buffer.
  // We built path like "/dir/file". We need "0:/dir/file".
  // Actually FatFs often accepts "/dir/file" if default drive is set, but better be explicit "0:"
  
  int path_len = (buf + size - 1) - p;
  memmove(buf + 2, p, path_len + 1); // +1 for null
  buf[0] = '0';
  buf[1] = ':';
  // buf[2] is already '/' from the loop or needs to be added?
  // loop adds '/' before name. So "/dir" -> "0:/dir"
  // Correct.
  
  return 0;
}

/*
 * File Operations
 */
static ssize_t fatfs_read(struct file *file, char *buf, size_t count, loff_t *ppos) {
  FIL *fp = (FIL *)file->private_data;
  UINT br;
  FRESULT res;

  if (!fp) return -EBADF;

  // Sync position? FatFs has internal ptr.
  // If VFS relies on *ppos, we should seek.
  if (f_tell(fp) != *ppos) {
    if (f_lseek(fp, *ppos) != FR_OK) {
        return -EIO;
    }
  }

  res = f_read(fp, buf, count, &br);
  if (res != FR_OK) {
      pr_err("FatFs read error: %d\r\n", res);
      return -EIO;
  }
  
  if (br > 0) {
      pr_info("FatFs: read %d bytes\r\n", br);
  } else {
      pr_info("FatFs: read EOF\r\n");
  }

  *ppos += br;
  return br;
}

static ssize_t fatfs_write(struct file *file, const char *buf, size_t count, loff_t *ppos) {
    FIL *fp = (FIL *)file->private_data;
    UINT bw;
    FRESULT res;

    if (!fp) return -EBADF;

    if (f_tell(fp) != *ppos) {
        if (f_lseek(fp, *ppos) != FR_OK) return -EIO;
    }

    res = f_write(fp, buf, count, &bw);
    if (res != FR_OK) return -EIO;

    *ppos += bw;
    return bw;
}

static int fatfs_release(struct inode *inode, struct file *file) {
    FIL *fp = (FIL *)file->private_data;
    if (fp) {
        f_close(fp);
        kfree(fp);
        file->private_data = NULL;
    }
    return 0;
}

static int fatfs_readdir(struct file *file, void *dirent, filldir_t filldir) {
    // FatFs readdir requires an open directory object (DIR).
    // File structure usually holds file handle. For dir, we need DIR handle.
    // In nos VFS current design, file->private_data can hold DIR* for directory.
    
    // WARNING: This depends on how VFS opens directories. 
    // If VFS calls open() on dir, we allocate DIR.
    DIR *dp = (DIR *)file->private_data;
    FILINFO fno;
    FRESULT res;
    int i = 0;

    if (!dp) return -EBADF;
    
    // FatFs doesn't support seekdir/rewinddir easily without reopen usually for sequence
    // But f_readdir continues from last state.
    
    // If f_pos is 0, we might need to rewind? 
    // f_readdir(dp, NULL) rewinds.
    if (file->f_pos == 0) {
        f_readdir(dp, NULL); 
    }

    while (1) {
        res = f_readdir(dp, &fno);
        if (res != FR_OK || fno.fname[0] == 0) break;

        // Skip . and ..? FatFs usually doesn't show them? Or does?
        // filldir expects specific format.
        
        filldir(dirent, fno.fname, strlen(fno.fname), file->f_pos, 0, 
                (fno.fattrib & AM_DIR) ? DT_DIR : DT_REG);
        
        file->f_pos++;
    }
    return 0;
}

/*
 * Inode Operations
 */

// Helper to alloc inode with ops
static struct inode *fatfs_make_inode(struct super_block *sb, int mode) {
    struct inode *inode = nos_vfs_alloc_inode(sb);
    if (!inode) return NULL;

    inode->i_mode = mode;
    inode->i_size = 0; // Populate from stat if possible
    
    if (mode & S_IFDIR) {
        inode->i_op = &fatfs_dir_inode_ops;
        inode->i_fop = &fatfs_dir_ops;
    } else {
        inode->i_op = &fatfs_file_inode_ops;
        inode->i_fop = &fatfs_file_ops;
    }
    return inode;
}


static struct dentry *fatfs_lookup(struct inode *dir, struct dentry *dentry) {
    // We need to verify existence.
    // Construct path for child.
    char path[256];
    FRESULT res;
    FILINFO fno;

    // Warning: dentry is not fully linked to tree yet usually during lookup?
    // Parent is linked. dentry->d_parent is parent.
    // We can construct path for 'dentry'.
    // get_fatfs_path expects dentry to be connected to root.
    // In lookup(dir, dentry), dentry is new child.
    
    
    if (get_fatfs_path(dentry, path, sizeof(path)) != 0) return NULL;

    res = f_stat(path, &fno);
    if (res == FR_OK) {
        // Found. Create inode.
        int mode = (fno.fattrib & AM_DIR) ? (S_IFDIR | 0755) : (S_IFREG | 0644);
        struct inode *inode = fatfs_make_inode(dir->i_sb, mode);
        if (inode) {
            inode->i_size = fno.fsize;
            dentry->d_inode = inode;
            return NULL; // Success
        }
    } else {
        // pr_err("FatFs: stat %s failed: %d\r\n", path, res); // Optional: hush noisy stat fails on lookup
    }
    
    return NULL; // Not found
}

static int fatfs_create(struct inode *dir, struct dentry *dentry, int mode) {
   // f_open with FA_CREATE_NEW or ALWAYS
   // But create just makes the file in VFS usually (or open calls create).
   // shell 'touch' or open(O_CREAT).
   char path[256];
   FRESULT res;
   FIL fp;
   
   if (get_fatfs_path(dentry, path, sizeof(path)) != 0) return -ENAMETOOLONG;
   
   res = f_open(&fp, path, FA_CREATE_NEW | FA_WRITE | FA_READ);
   if (res == FR_OK) {
       f_close(&fp);
       struct inode *inode = fatfs_make_inode(dir->i_sb, mode);
       if (inode) {
           dentry->d_inode = inode;
           return 0;
       }
   }
   return -EIO;
}

static int fatfs_mkdir(struct inode *dir, struct dentry *dentry, int mode) {
    char path[256];
    FRESULT res;
    
    if (get_fatfs_path(dentry, path, sizeof(path)) != 0) return -ENAMETOOLONG;
    
    res = f_mkdir(path);
    if (res == FR_OK) {
        struct inode *inode = fatfs_make_inode(dir->i_sb, S_IFDIR | 0755);
        if (inode) {
            dentry->d_inode = inode;
            return 0;
        }
    } else {
        pr_err("FatFs: mkdir failed: %d\r\n", res);
    }
    return -EIO;
}


// File Open



static int fatfs_dir_release(struct inode *inode, struct file *file) {
    DIR *dp = (DIR *)file->private_data;
    if (dp) {
        f_closedir(dp);
        kfree(dp);
        file->private_data = NULL;
    }
    return 0;
}

static int fatfs_dir_open(struct inode *inode, struct file *file) {
    DIR *dp;
    char path[256];
    FRESULT res;

    // Resolve path using dentry
    if (!file->f_path.dentry) {
        // Fallback or error?
        // RamFS didn't use dentry for open, but FatFs needs it for path.
        // shell cmd_ls must provide it.
        return -EINVAL;
    }

    if (get_fatfs_path(file->f_path.dentry, path, sizeof(path)) != 0)
        return -ENAMETOOLONG;

    dp = kzalloc(sizeof(DIR), GFP_KERNEL);
    if (!dp) return -ENOMEM;

    res = f_opendir(dp, path);
    if (res != FR_OK) {
        kfree(dp);
        return -ENOENT;
    }

    file->private_data = dp;
    return 0;
}

/* Ops Setup */
static struct inode_operations fatfs_dir_inode_ops = {
    .lookup = fatfs_lookup,
    .create = fatfs_create,
    .mkdir = fatfs_mkdir,
};

static struct inode_operations fatfs_file_inode_ops = {
    // Truncate?
};

static int fatfs_file_open(struct inode *inode, struct file *file) {
    FIL *fp;
    char path[256];
    FRESULT res;

    // Resolve path using dentry
    if (!file->f_path.dentry) {
        return -EINVAL;
    }

    if (get_fatfs_path(file->f_path.dentry, path, sizeof(path)) != 0)
        return -ENAMETOOLONG;

    fp = kzalloc(sizeof(FIL), GFP_KERNEL);
    if (!fp) return -ENOMEM;

    // Open for Read/Write. VFS handles permission check usually?
    // NOS VFS is simple. Open with FA_READ | FA_WRITE?
    // Depends on flags. file->f_flags.
    // For now simple read/write.
    res = f_open(fp, path, FA_READ | FA_WRITE);
    if (res != FR_OK) {
        // Try read only
        res = f_open(fp, path, FA_READ);
        if (res != FR_OK) {
            // pr_err("FatFs: open %s failed: %d\r\n", path, res);
            kfree(fp);
            return -ENOENT;
        }
    }
    
    file->private_data = fp;
    return 0;
}

static struct file_operations fatfs_file_ops = {
    .read = fatfs_read,
    .write = fatfs_write,
    .release = fatfs_release,
    .open = fatfs_file_open,
};

static struct file_operations fatfs_dir_ops = {
    .open = fatfs_dir_open,
    .readdir = fatfs_readdir,
    .release = fatfs_dir_release,
};


/*
 * Superblock / Mount
 */
static int fatfs_fill_super(struct super_block *sb, void *data, int silent) {
    // Mount FATFS
    FATFS *fs = kzalloc(sizeof(FATFS), GFP_KERNEL);
    if (!fs) return -ENOMEM;
    
    // Mount "0:"
    FRESULT res = f_mount(fs, "0:", 1);
    if (res == FR_NO_FILESYSTEM) {
        pr_info("FatFs: No filesystem found on 0:, formatting...\r\n");
        // Create FAT volume
        // f_mkfs(path, sfd, au) - sfd=0 (legacy), au=0 (auto)
        // Note: Check ff.h for f_mkfs signature. It varies by version.
        // Assuming R0.11a from ff.h viewed earlier: FRESULT f_mkfs (const TCHAR* path, BYTE sfd, UINT au);
        res = f_mkfs("0:", 0, 0); 
        if (res != FR_OK) {
             pr_err("FatFs: mkfs failed: %d\r\n", res);
             kfree(fs);
             return -EIO;
        }
        // Retry mount
        res = f_mount(fs, "0:", 1);
    }
    
    if (res != FR_OK) {
        pr_err("FatFs: mount failed: %d\r\n", res);
        kfree(fs);
        return -EIO;
    }
    
    sb->s_fs_info = fs;
    
    // Root inode
    struct inode *inode = fatfs_make_inode(sb, S_IFDIR | 0755);
    if (!inode) return -ENOMEM;
    
    struct dentry *root = kzalloc(sizeof(struct dentry), GFP_KERNEL);
    if (!root) return -ENOMEM;
    
    root->d_inode = inode;
    root->d_sb = sb;
    sb->s_root = root;
    
    return 0;
}

static struct dentry *fatfs_mount(struct file_system_type *fs_type, int flags,
                                  const char *dev_name, void *data) {
    struct super_block *sb = kzalloc(sizeof(struct super_block), GFP_KERNEL);
    if (!sb) return NULL;
    
    if (fatfs_fill_super(sb, data, flags)) {
        kfree(sb);
        return NULL;
    }
    return sb->s_root;
}

static struct file_system_type fatfs_fs_type = {
    .name = "fatfs",
    .mount = fatfs_mount,
};

int init_fatfs(void) {
    return register_filesystem(&fatfs_fs_type);
}
