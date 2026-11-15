/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <assert.h>
#include <stdlib.h>
#include <kernel/device.h>

const struct device *device_get_binding(const char *name);
#include "../../fs/littlefs/littlefs_vfs.c"

enum { BLOCK_SIZE = 4096, BLOCK_COUNT = 37, SECTOR_SIZE = 512 };
static uint8_t flash[BLOCK_SIZE * BLOCK_COUNT];

static int flash_read(const struct device *dev, uint32_t lba, void *buf, uint32_t count)
{
    (void)dev;
    memcpy(buf, flash + lba * SECTOR_SIZE, count * SECTOR_SIZE);
    return 0;
}

static int flash_prog(const struct device *dev, uint32_t lba, const void *buf, uint32_t count)
{
    const uint8_t *src = buf;
    (void)dev;
    for (uint32_t i = 0; i < count * SECTOR_SIZE; i++) {
        flash[lba * SECTOR_SIZE + i] &= src[i];
    }
    return 0;
}

static int flash_erase(const struct device *dev, uint32_t lba, uint32_t count)
{
    (void)dev;
    memset(flash + lba * SECTOR_SIZE, 0xff, count * SECTOR_SIZE);
    return 0;
}

static int flash_info(const struct device *dev, uint32_t *sector_size, uint32_t *sector_count)
{
    (void)dev;
    *sector_size = SECTOR_SIZE;
    *sector_count = sizeof(flash) / SECTOR_SIZE;
    return 0;
}

const struct device *device_get_binding(const char *name)
{
    static const struct block_driver_api api = {flash_read, flash_prog, flash_erase, flash_info};
    static const struct device dev = {.api = &api, .ready = true};

    return strcmp(name, "data-test") == 0 ? &dev : NULL;
}

void *kmalloc(size_t size, gfp_t flags) { (void)flags; return malloc(size); }
void *kzalloc(size_t size, gfp_t flags) { (void)flags; return calloc(1, size); }
void kfree(void *ptr) { free(ptr); }
int register_filesystem(struct file_system_type *fs) { (void)fs; return 0; }
int vfs_dentry_get(struct dentry *dentry) { (void)dentry; return 0; }
void vfs_dentry_put(struct dentry *dentry) { (void)dentry; }

struct inode *nos_vfs_alloc_inode(struct super_block *sb)
{
    struct inode *inode = calloc(1, sizeof(*inode));
    assert(inode != NULL);
    inode->i_sb = sb;
    return inode;
}

int pr_log(bool no_tag, enum log_level level, const char *fmt, ...)
{
    (void)no_tag;
    (void)level;
    (void)fmt;
    return 0;
}

int main(void)
{
    static const char payload[2 * BLOCK_SIZE];
    struct dentry *root;
    struct super_block *sb;
    struct littlefs_sb_info *sbi;
    struct kstatfs stats;
    lfs_file_t file;

    memset(flash, 0xff, sizeof(flash));
    root = littlefs_fs_type.mount(&littlefs_fs_type, 0, "data-test", NULL);
    assert(root != NULL);
    sb = root->d_sb;
    sbi = sb->s_fs_info;
    assert(sb->s_op->statfs != NULL);

    assert(sb->s_op->statfs(root, &stats) == 0);
    assert(stats.bsize == BLOCK_SIZE && stats.blocks == BLOCK_COUNT);
    assert(stats.bfree == BLOCK_COUNT - 2 && stats.bavail == stats.bfree);

    /* A directory pair can be visited twice, but only uses two blocks. */
    assert(lfs_mkdir(&sbi->lfs, "/dir") == 0);
    assert(sb->s_op->statfs(root, &stats) == 0);
    assert(stats.bfree == BLOCK_COUNT - 4);

    assert(lfs_file_open(&sbi->lfs, &file, "/dir/file", LFS_O_CREAT | LFS_O_WRONLY) == 0);
    assert(lfs_file_write(&sbi->lfs, &file, payload, sizeof(payload)) == sizeof(payload));
    assert(lfs_file_close(&sbi->lfs, &file) == 0);
    assert(sb->s_op->statfs(root, &stats) == 0);
    assert(stats.bfree < BLOCK_COUNT - 4);

    assert(lfs_remove(&sbi->lfs, "/dir/file") == 0);
    assert(sb->s_op->statfs(root, &stats) == 0);
    assert(stats.bfree == BLOCK_COUNT - 4 && stats.bavail == stats.bfree);

    littlefs_fs_type.kill_sb(sb);
    free(root->d_inode);
    free(root);
    free(sb);
    return 0;
}
