/**
 * Copyright (C) 2024-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <fs/fs.h>
#include <fs/vfs.h>
#include <kernel/device.h>
#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <kernel/printk.h>
#include <lib/vsprintf.h>
#include <string.h>

#define SYSFS_ROOT_INO 1
#define SYSFS_ARCH_INO 2
#define SYSFS_BOARD_INO 3
#define SYSFS_CPU_INO 4
#define SYSFS_VERSION_INO 5
#define SYSFS_DEVICES_INO 6

struct sysfs_entry {
    const char *name;
    int mode;
    int ino;
};

static const struct sysfs_entry sysfs_files[] = {
    { "arch",    S_IFREG | 0444, SYSFS_ARCH_INO },
    { "board",   S_IFREG | 0444, SYSFS_BOARD_INO },
    { "cpu",     S_IFREG | 0444, SYSFS_CPU_INO },
    { "version", S_IFREG | 0444, SYSFS_VERSION_INO },
    { "devices", S_IFREG | 0444, SYSFS_DEVICES_INO },
    { NULL, 0, 0 }
};

struct sysfs_dev_buf {
    char *buf;
    size_t size;
    int pos;
};

static int sysfs_dev_iter(struct device *dev, void *arg)
{
    struct sysfs_dev_buf *db = arg;
    int n;

    if ((size_t)db->pos >= db->size) {
        return 1;
    }
    n = snprintf(db->buf + db->pos, db->size - (size_t)db->pos,
                 "%-16s ready=%u ref=%u init_res=%d\n",
                 dev->name ? dev->name : "unnamed",
                 (unsigned int)dev->ready,
                 (unsigned int)dev->ref_count,
                 dev->init_res);
    if (n < 0 || (size_t)(db->pos + n) >= db->size) {
        db->pos = (int)db->size - 1;
        return 1;
    }
    db->pos += n;
    return 0;
}

static ssize_t sysfs_read(struct file *file, char *buf, size_t count, loff_t *ppos)
{
    struct inode *inode = file->f_inode;
    char *page;
    size_t page_size = 512;
    int len = 0;

    page = kmalloc(page_size, GFP_KERNEL);
    if (page == NULL) {
        return -ENOMEM;
    }
    switch (inode->i_ino) {
    case SYSFS_ARCH_INO:
        len = snprintf(page, page_size, "%s\n", CONFIG_ARCH);
        break;
    case SYSFS_BOARD_INO:
        len = snprintf(page, page_size, "%s\n", CONFIG_BOARD);
        break;
    case SYSFS_CPU_INO:
        len = snprintf(page, page_size, "%s\n", CONFIG_CPU_TYPE);
        break;
    case SYSFS_VERSION_INO:
        len = snprintf(page, page_size, "%u.%u.%u\n",
                       (unsigned int)((CONFIG_VERSION_CODE >> 16) & 0xffU),
                       (unsigned int)((CONFIG_VERSION_CODE >> 8) & 0xffU),
                       (unsigned int)(CONFIG_VERSION_CODE & 0xffU));
        break;
    case SYSFS_DEVICES_INO: {
        struct sysfs_dev_buf db = { .buf = page, .size = page_size, .pos = 0 };
        device_foreach(sysfs_dev_iter, &db);
        len = db.pos;
        break;
    }
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
    if (count > (size_t)(len - *ppos)) {
        count = (size_t)(len - *ppos);
    }
    memcpy(buf, page + *ppos, count);
    *ppos += count;
    kfree(page);
    return (ssize_t)count;
}

static struct file_operations sysfs_file_ops = {
    .read = sysfs_read,
};

/*
 * File Operations
 */
static int sysfs_readdir(struct file *file, void *dirent, filldir_t filldir) {
    struct inode *inode = file->f_inode;
    int i;

    if (inode->i_ino != SYSFS_ROOT_INO) {
        return -ENOTDIR;
    }
    for (i = 0; sysfs_files[i].name != NULL; i++) {
        if (i >= file->f_pos) {
            if (filldir(dirent, sysfs_files[i].name,
                        (int)strlen(sysfs_files[i].name),
                        i, (u64)sysfs_files[i].ino, DT_REG)) {
                break;
            }
            file->f_pos++;
        }
    }
    return 0;
}

static struct file_operations sysfs_dir_ops = {
    .readdir = sysfs_readdir,
};

/*
 * Inode Operations
 */
static struct dentry *sysfs_lookup(struct inode *dir, struct dentry *dentry) {
    int i;

    if (dir->i_ino != SYSFS_ROOT_INO) {
        return NULL;
    }
    for (i = 0; sysfs_files[i].name != NULL; i++) {
        if (strcmp((const char *)dentry->d_name.name, sysfs_files[i].name) == 0) {
            struct inode *inode = nos_vfs_alloc_inode(dir->i_sb);
            if (!inode) {
                return NULL;
            }
            inode->i_ino = sysfs_files[i].ino;
            inode->i_mode = sysfs_files[i].mode;
            inode->i_nlink = 1;
            inode->i_size = 0;
            inode->i_op = NULL;
            inode->i_fop = &sysfs_file_ops;
            dentry->d_inode = inode;
            return NULL;
        }
    }
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
