/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NuTiny-NUC472 板级 LittleFS 挂载管理 (/data)
 */

#include <kernel/kernel.h>
#include <kernel/init.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <fs/fs.h>
#include <fs/vfs.h>

int nutiny_fs_init(void)
{
    struct dentry *root;
    struct dentry *dir = NULL;
    struct dentry *mnt_root;

    root = vfs_get_root();
    if (root == NULL) {
        pr_err("nutiny_fs: VFS root not initialized\n");
        return -ENOENT;
    }

    /* 确保 /data 挂载点目录存在 */
    (void)vfs_make_dir(root, "data", 0755, &dir);
    if (dir == NULL) {
        dir = vfs_lookup_path(root, "/data");
    }
    if (dir == NULL) {
        pr_err("nutiny_fs: failed to create /data directory\n");
        return -ENOENT;
    }

    /* 挂载 LittleFS 块设备 */
    mnt_root = vfs_mount("littlefs", 0, "nuc472-lfs", NULL);
    if (mnt_root == NULL) {
        pr_err("nutiny_fs: failed to mount littlefs on nuc472-lfs\n");
        return -EIO;
    }

    if (vfs_bind_mount(dir, mnt_root) != 0) {
        pr_err("nutiny_fs: failed to bind /data mount\n");
        return -EIO;
    }

    pr_info("nutiny_fs: mounted LittleFS at /data (128 KB)\n");
    return 0;
}
SYS_INIT(nutiny_fs_init, INIT_LEVEL_APPLICATION);
