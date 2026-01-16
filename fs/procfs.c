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
#include <kernel/cpu.h>
#include <kernel/task.h>
#include <kernel/printk.h>
#include <string.h>
#include <lib/vsprintf.h>

/*
 * Procfs is a pseudo-filesystem. 
 * Inodes are dynamically generated or static for root.
 * We use inode numbers to distinguish files.
 */

#define PROC_ROOT_INO 1
#define PROC_VERSION_INO 2
#define PROC_UPTIME_INO 3
#define PROC_MEMINFO_INO 4

struct proc_entry {
    const char *name;
    int mode;
    int ino;
};

static const struct proc_entry proc_files[] = {
    { "version", S_IFREG | 0444, PROC_VERSION_INO },
    { "uptime",  S_IFREG | 0444, PROC_UPTIME_INO },
    { "meminfo", S_IFREG | 0444, PROC_MEMINFO_INO },
    { NULL, 0, 0 }
};

/*
 * Content Generation
 */
static int proc_read_version(char *buf, size_t size) {
    return snprintf(buf, size, "NOS Kernel version %d.%d.%d build %s\n", 
                    (CONFIG_VERSION_CODE >> 16) & 0xff,
                    (CONFIG_VERSION_CODE >> 8) & 0xff,
                    CONFIG_VERSION_CODE & 0xff,
                    __DATE__ " " __TIME__);
}

static int proc_read_uptime(char *buf, size_t size) {
    u64 ticks = cpu_run_ticks();
    // Assuming 1000 ticks per second (CONFIG_SYS_TICK_MS usually 1?)
    // Need to check tick rate. Assuming 1ms per tick from common configs.
    u32 seconds = ticks / 1000; 
    u32 centiseconds = (ticks % 1000) / 10;
    return snprintf(buf, size, "%u.%02u\n", seconds, centiseconds);
}

static int proc_read_meminfo(char *buf, size_t size) {
    u32 free_pages = mm_get_free_page_num();
    u32 total_pages = mm_get_total_page_num();
    u32 page_size = CONFIG_PAGE_SIZE; 
    
    // Convert to kB
    u32 mem_total_kb = (total_pages * page_size) / 1024;
    u32 mem_free_kb = (free_pages * page_size) / 1024;
    
    return snprintf(buf, size, 
        "MemTotal:       %8u kB\n"
        "MemFree:        %8u kB\n", 
        mem_total_kb, mem_free_kb);
}

/*
 * File Operations
 */
static ssize_t procfs_read(struct file *file, char *buf, size_t count, loff_t *ppos) {
    struct inode *inode = file->f_inode;
    char page[512]; // Small buffer for simple stats
    int len = 0;
    
    switch (inode->i_ino) {
        case PROC_VERSION_INO:
            len = proc_read_version(page, sizeof(page));
            break;
        case PROC_UPTIME_INO:
            len = proc_read_uptime(page, sizeof(page));
            break;
        case PROC_MEMINFO_INO:
            len = proc_read_meminfo(page, sizeof(page));
            break;
        default:
            return 0;
    }
    
    if (*ppos >= len)
        return 0;
        
    if (count > len - *ppos)
        count = len - *ppos;
        
    memcpy(buf, page + *ppos, count);
    *ppos += count;
    return count;
}

static struct file_operations procfs_file_ops = {
    .read = procfs_read,
};

static int procfs_readdir(struct file *file, void *dirent, filldir_t filldir) {
    struct inode *inode = file->f_inode;
    int i = 0;
    
    // pr_info("procfs_readdir: ino=%lu pos=%lld\r\n", inode->i_ino, file->f_pos);

    if (inode->i_ino != PROC_ROOT_INO)
        return -ENOTDIR;

    // Iterate over static entries
    for (i = 0; proc_files[i].name != NULL; i++) {
        if (i >= file->f_pos) {
            // pr_info("procfs_readdir: filling %s\r\n", proc_files[i].name);
            if (filldir(dirent, proc_files[i].name, strlen(proc_files[i].name), 
                        i, proc_files[i].ino, DT_REG))
                break;
            file->f_pos++;
        }
    }
    return 0;
}

static struct file_operations procfs_dir_ops = {
    .readdir = procfs_readdir,
};

/*
 * Inode Operations
 */
static struct dentry *procfs_lookup(struct inode *dir, struct dentry *dentry) {
    int i;
    // pr_info("procfs_lookup: %s\r\n", dentry->d_name.name);

    if (dir->i_ino != PROC_ROOT_INO)
        return NULL;
        
    for (i = 0; proc_files[i].name != NULL; i++) {
        if (strcmp(dentry->d_name.name, proc_files[i].name) == 0) {
            // Found
            struct inode *inode = nos_vfs_alloc_inode(dir->i_sb);
            if (!inode) return NULL;
            
            inode->i_ino = proc_files[i].ino;
            inode->i_mode = proc_files[i].mode;
            inode->i_size = 0; // Dynamic size
            
            if (S_ISDIR(inode->i_mode)) {
                inode->i_op = NULL; // No subdirs for now
                inode->i_fop = &procfs_dir_ops;
            } else {
                inode->i_op = NULL;
                inode->i_fop = &procfs_file_ops;
            }
            
            dentry->d_inode = inode;
            // pr_info("procfs_lookup: found %s\r\n", proc_files[i].name);
            return NULL;
        }
    }
    return NULL;
}

static struct inode_operations procfs_dir_inode_ops = {
    .lookup = procfs_lookup,
};

/*
 * Superblock Operations
 */
static int procfs_fill_super(struct super_block *sb, void *data, int silent) {
    struct inode *inode;
    struct dentry *root;

    sb->s_blocksize = 1024;
    sb->s_blocksize_bits = 10;
    sb->s_op = NULL; // Default ops?

    inode = nos_vfs_alloc_inode(sb);
    if (!inode) return -ENOMEM;
    
    inode->i_ino = PROC_ROOT_INO;
    inode->i_mode = S_IFDIR | 0755;
    inode->i_op = &procfs_dir_inode_ops;
    inode->i_fop = &procfs_dir_ops;

    root = kzalloc(sizeof(struct dentry), GFP_KERNEL);
    if (!root) return -ENOMEM;

    root->d_inode = inode;
    root->d_sb = sb;
    sb->s_root = root;

    return 0;
}

static struct dentry *procfs_mount(struct file_system_type *fs_type, int flags,
                                  const char *dev_name, void *data) {
    struct super_block *sb = kzalloc(sizeof(struct super_block), GFP_KERNEL);
    if (!sb) return NULL;
    
    // pr_info("procfs: mount called\r\n");

    if (procfs_fill_super(sb, data, flags)) {
        kfree(sb);
        return NULL;
    }
    return sb->s_root;
}

static struct file_system_type procfs_fs_type = {
    .name = "procfs",
    .mount = procfs_mount,
};

int init_procfs(void) {
    return register_filesystem(&procfs_fs_type);
}
