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
#define PROC_CPUINFO_INO 5
#define PROC_MOUNTS_INO 6
#define PROC_FILESYSTEMS_INO 7
#define PROC_TASKS_INO 8

struct proc_entry {
    const char *name;
    int mode;
    int ino;
};

static const struct proc_entry proc_files[] = {
    { "version",     S_IFREG | 0444, PROC_VERSION_INO },
    { "uptime",      S_IFREG | 0444, PROC_UPTIME_INO },
    { "meminfo",     S_IFREG | 0444, PROC_MEMINFO_INO },
    { "cpuinfo",     S_IFREG | 0444, PROC_CPUINFO_INO },
    { "mounts",      S_IFREG | 0444, PROC_MOUNTS_INO },
    { "filesystems", S_IFREG | 0444, PROC_FILESYSTEMS_INO },
    { "tasks",       S_IFREG | 0444, PROC_TASKS_INO },
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
                    CONFIG_BUILD_INFO);
}

static int proc_read_uptime(char *buf, size_t size) {
    u64 ticks = cpu_run_ticks();
    u32 seconds = ticks / 1000; 
    u32 centiseconds = (ticks % 1000) / 10;
    return snprintf(buf, size, "%u.%02u\n", seconds, centiseconds);
}

static int proc_read_meminfo(char *buf, size_t size) {
    u32 free_pages = mm_get_free_page_num();
    u32 total_pages = mm_get_total_page_num();
    u32 page_size = CONFIG_PAGE_SIZE; 
    u32 mem_total_kb = (total_pages * page_size) / 1024;
    u32 mem_free_kb = (free_pages * page_size) / 1024;
    u32 mem_used_kb = mem_total_kb >= mem_free_kb ? (mem_total_kb - mem_free_kb) : 0;
    u32 heap_total = (u32)mm_block_total_size();
    u32 heap_used = (u32)mm_block_used_size();
    u32 heap_free = (u32)mm_block_free_size();
    
    return snprintf(buf, size, 
        "MemTotal:       %8u kB\n"
        "MemFree:        %8u kB\n"
        "MemUsed:        %8u kB\n"
        "PageSize:       %8u B\n"
        "HeapTotal:      %8u B\n"
        "HeapUsed:       %8u B\n"
        "HeapFree:       %8u B\n",
        mem_total_kb, mem_free_kb, mem_used_kb, page_size,
        heap_total, heap_used, heap_free);
}

static int proc_read_cpuinfo(char *buf, size_t size)
{
    return snprintf(buf, size,
        "processor\t: 0\n"
        "arch\t\t: %s\n"
        "cpu\t\t: %s\n"
        "board\t\t: %s\n"
        "tick_ms\t\t: %u\n",
        CONFIG_ARCH, CONFIG_CPU_TYPE, CONFIG_BOARD, (unsigned int)CONFIG_SYS_TICK_MS);
}

static int proc_read_mounts(char *buf, size_t size)
{
    int count = vfs_get_mount_count();
    int pos = 0;
    int i;

    for (i = 0; i < count; i++) {
        struct vfs_mount_entry entry;
        int n;
        if (vfs_get_mount_entry(i, &entry) != 0) {
            continue;
        }
        n = snprintf(buf + pos, size - (size_t)pos, "%s %s %s rw 0 0\n",
                     entry.dev_name ? entry.dev_name : "none",
                     entry.mount_path ? entry.mount_path : "/",
                     entry.fs_name ? entry.fs_name : "unknown");
        if (n < 0 || (size_t)(pos + n) >= size) {
            pos = (int)size - 1;
            break;
        }
        pos += n;
    }
    return pos;
}

struct proc_task_buf {
    char *buf;
    size_t size;
    int pos;
};

static const char *proc_task_state(uint8_t status)
{
    switch (status) {
    case TASK_RUNING:  return "R";
    case TASK_READY:   return "R";
    case TASK_WAIT:    return "S";
    case TASK_SUSPEND: return "T";
    case TASK_CLOSE:   return "Z";
    default:           return "?";
    }
}

static int proc_task_iter(struct task_struct *task, void *arg)
{
    struct proc_task_buf *tb = arg;
    int n;

    if ((size_t)tb->pos >= tb->size) {
        return 1;
    }
    n = snprintf(tb->buf + tb->pos, tb->size - (size_t)tb->pos,
                 "%4u  %3u  %-5s %6u %6u  %s\n",
                 (unsigned int)task->pid,
                 (unsigned int)task->current_priority,
                 proc_task_state(task->status),
                 (unsigned int)task->stack_size,
                 (unsigned int)task_stack_max_used(task),
                 task->name ? task->name : "-");
    if (n < 0 || (size_t)(tb->pos + n) >= tb->size) {
        tb->pos = (int)tb->size - 1;
        return 1;
    }
    tb->pos += n;
    return 0;
}

static int proc_read_tasks(char *buf, size_t size)
{
    struct proc_task_buf tb = { .buf = buf, .size = size, .pos = 0 };
    int n = snprintf(buf, size, " PID PRIO STATE  STACK   USED  NAME\n");
    if (n > 0 && (size_t)n < size) {
        tb.pos = n;
        task_foreach(proc_task_iter, &tb);
    }
    return tb.pos;
}

/*
 * File Operations
 */
static ssize_t procfs_read(struct file *file, char *buf, size_t count, loff_t *ppos) {
    struct inode *inode = file->f_inode;
    char *page;
    size_t page_size = 1024;
    int len = 0;

    page = kmalloc(page_size, GFP_KERNEL);
    if (page == NULL) {
        return -ENOMEM;
    }
    
    switch (inode->i_ino) {
        case PROC_VERSION_INO:
            len = proc_read_version(page, page_size);
            break;
        case PROC_UPTIME_INO:
            len = proc_read_uptime(page, page_size);
            break;
        case PROC_MEMINFO_INO:
            len = proc_read_meminfo(page, page_size);
            break;
        case PROC_CPUINFO_INO:
            len = proc_read_cpuinfo(page, page_size);
            break;
        case PROC_MOUNTS_INO:
            len = proc_read_mounts(page, page_size);
            break;
        case PROC_FILESYSTEMS_INO:
            len = vfs_list_filesystems(page, page_size);
            break;
        case PROC_TASKS_INO:
            len = proc_read_tasks(page, page_size);
            break;
        default:
            kfree(page);
            return 0;
    }
    if (len < 0) {
        kfree(page);
        return len;
    }
    if ((size_t)len >= page_size) {
        len = (int)(page_size - 1);
    }
    
    if (*ppos >= len) {
        kfree(page);
        return 0;
    }
        
    if (count > len - *ppos)
        count = len - *ppos;
        
    memcpy(buf, page + *ppos, count);
    *ppos += count;
    kfree(page);
    return count;
}

static struct file_operations procfs_file_ops = {
    .read = procfs_read,
};

static int procfs_readdir(struct file *file, void *dirent, filldir_t filldir) {
    struct inode *inode = file->f_inode;
    int i = 0;
    
    // pr_info("procfs_readdir: ino=%lu pos=%lld\n", inode->i_ino, file->f_pos);

    if (inode->i_ino != PROC_ROOT_INO)
        return -ENOTDIR;

    // Iterate over static entries
    for (i = 0; proc_files[i].name != NULL; i++) {
        if (i >= file->f_pos) {
            // pr_info("procfs_readdir: filling %s\n", proc_files[i].name);
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
    // pr_info("procfs_lookup: %s\n", dentry->d_name.name);

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
            // pr_info("procfs_lookup: found %s\n", proc_files[i].name);
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
    
    // pr_info("procfs: mount called\n");

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
