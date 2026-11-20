/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * Author: Nick Hu
 */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/device.h>
#include <kernel/mm.h>
#include <nos/keyboard_object_store.h>

uintptr_t kernel_running;
static unsigned int allocations;

void *kmalloc(size_t size, gfp_t flags)
{
    void *ptr = malloc(size);
    (void)flags;
    if (ptr != NULL) allocations++;
    return ptr;
}

void *kzalloc(size_t size, gfp_t flags)
{
    void *ptr = kmalloc(size, flags);
    if (ptr != NULL) memset(ptr, 0, size);
    return ptr;
}

void kfree(void *ptr)
{
    if (ptr != NULL) {
        assert(allocations != 0U);
        allocations--;
        free(ptr);
    }
}

char *kstrdup(const char *str, gfp_t flags)
{
    size_t length = strlen(str) + 1U;
    char *copy = kmalloc(length, flags);
    if (copy != NULL) memcpy(copy, str, length);
    return copy;
}

/* Exercise the production descriptor table before scheduler startup. */
#define __NOS_TASK_H__
struct task_struct { struct file *files[8]; };
static struct task_struct *task_current(void) { return NULL; }
const struct device *device_get_binding(const char *name);

#include "../../fs/vfs.c"
#include "../../fs/vfs_fd.c"
#include "../../fs/vfs_io.c"
#include "../../fs/littlefs/littlefs_vfs.c"
#include <stdio.h>

enum { BLOCK_SIZE = 4096, BLOCK_COUNT = 128, SECTOR_SIZE = 512 };
static uint8_t flash[BLOCK_SIZE * BLOCK_COUNT];
static uint8_t saved_flash[sizeof(flash)];
static unsigned int erases;

static int flash_read(const struct device *dev, uint32_t lba, void *buf, uint32_t count)
{
    (void)dev;
    assert((size_t)lba + count <= sizeof(flash) / SECTOR_SIZE);
    memcpy(buf, flash + lba * SECTOR_SIZE, count * SECTOR_SIZE);
    return 0;
}

static int flash_prog(const struct device *dev, uint32_t lba, const void *buf, uint32_t count)
{
    const uint8_t *src = buf;
    (void)dev;
    assert((size_t)lba + count <= sizeof(flash) / SECTOR_SIZE);
    for (uint32_t i = 0; i < count * SECTOR_SIZE; i++) {
        assert((flash[lba * SECTOR_SIZE + i] & src[i]) == src[i]);
        flash[lba * SECTOR_SIZE + i] &= src[i];
    }
    return 0;
}

static int flash_erase(const struct device *dev, uint32_t lba, uint32_t count)
{
    (void)dev;
    assert((size_t)lba + count <= sizeof(flash) / SECTOR_SIZE);
    memset(flash + lba * SECTOR_SIZE, 0xff, count * SECTOR_SIZE);
    erases++;
    return 0;
}

static int flash_info(const struct device *dev, uint32_t *size, uint32_t *count)
{
    (void)dev;
    *size = SECTOR_SIZE;
    *count = sizeof(flash) / SECTOR_SIZE;
    return 0;
}

const struct device *device_get_binding(const char *name)
{
    static const struct block_driver_api api = {flash_read, flash_prog, flash_erase, flash_info};
    static const struct device dev = {.api = &api, .ready = true};
    return strcmp(name, "keyboard-test") == 0 ? &dev : NULL;
}

int pr_log(bool no_tag, enum log_level level, const char *fmt, ...)
{
    (void)no_tag;
    (void)level;
    (void)fmt;
    return 0;
}

static void mount_volume(void)
{
    root_dentry = littlefs_fs_type.mount(&littlefs_fs_type, 0, "keyboard-test", NULL);
    assert(root_dentry != NULL);
    root_sb = root_dentry->d_sb;
}

static void free_cached_tree(struct dentry *dentry)
{
    vfs_ensure_dentry_lists(dentry);
    while (!list_empty(&dentry->d_subdirs)) {
        struct dentry *child = list_first_entry(&dentry->d_subdirs, struct dentry, d_child);
        list_del(&child->d_child);
        free_cached_tree(child);
    }
    assert(dentry->d_refs == 1U);
    nos_vfs_destroy_inode(dentry->d_inode);
    kfree(dentry);
}

static void unmount_volume(void)
{
    for (unsigned int i = 0; i < CONFIG_TASK_MAX_FDS; i++) assert(early_files[i] == NULL);
    littlefs_fs_type.kill_sb(root_sb);
    free_cached_tree(root_dentry);
    kfree(root_sb);
    root_dentry = NULL;
    root_sb = NULL;
    assert(allocations == 0U);
}

static void expect_body(struct kb_store *store, uint8_t kind, const char *id, const char *expected)
{
    char body[32] = {0};
    size_t length = strlen(expected);
    assert(kb_store_read(store, kind, id, KB_STORE_CURRENT_REVISION, 0,
                         body, sizeof(body)) == (int)length);
    assert(memcmp(body, expected, length) == 0);
}

int main(void)
{
    struct kb_store store, reopened;
    struct kb_store_selection selection;
    uint64_t old_revision, current_sequence;
    unsigned int before_erases;
    int fd;
    char byte;

    memset(flash, 0xff, sizeof(flash));
    mount_volume();
    assert(nos_mkdir("/data", 0755) == 0);
    assert(kb_store_open(&store) == -ENOENT);
    assert(kb_store_seed(&store, "base", 4, "light", 5) == 0);
    assert(kb_store_open(&reopened) == 0);
    expect_body(&reopened, KB_STORE_BASE, "default", "base");
    expect_body(&reopened, KB_STORE_LIGHT, "default", "light");
    /* 验证扁平文件结构存在且无 _catalog 目录 */
    fd = nos_open("/data/keyboard/base/default.nkc", O_RDONLY);
    assert(fd >= 0 && nos_close(fd) == 0);
    fd = nos_open("/data/keyboard/light/default.nkc", O_RDONLY);
    assert(fd >= 0 && nos_close(fd) == 0);
    assert(nos_open("/data/keyboard/_catalog", O_RDONLY) == -ENOENT);

    old_revision = store.selection.base.revision;
    assert(kb_store_save(&store, KB_STORE_BASE, "default", old_revision,
                         "updated", 7, NULL) == 0);
    /* 覆盖写直接替换扁平对象文件，旧修订立即不可再读 */
    assert(kb_store_read(&store, KB_STORE_BASE, "default", old_revision, 0, &byte, 1) == -ENOENT);
    assert(kb_store_save(&store, KB_STORE_MACRO, "macro", 0, "steps", 5, NULL) == 0);
    assert(kb_store_save(&store, KB_STORE_MACRO, "\xe6\xb5\x8b\xe8\xaf\x95\xe5\xae\x8f", 0,
                         "utf8", 4, NULL) == 0);
    selection = store.selection;
    selection.light_enabled = false;
    assert(kb_store_persist_selection(&store, &selection) == 0);
    assert(kb_store_open(&reopened) == 0);
    expect_body(&reopened, KB_STORE_BASE, "default", "updated");
    expect_body(&reopened, KB_STORE_MACRO, "macro", "steps");
    expect_body(&reopened, KB_STORE_MACRO, "\xe6\xb5\x8b\xe8\xaf\x95\xe5\xae\x8f", "utf8");
    assert(kb_store_gc(&store) == 0);
    assert(kb_store_read(&store, KB_STORE_BASE, "default", old_revision, 0, &byte, 1) == -ENOENT);
    current_sequence = store.sequence;
    unmount_volume();

    /* Remount from the same flash bytes, without retaining VFS cache state. */
    mount_volume();
    assert(kb_store_open(&reopened) == 0 && reopened.sequence == current_sequence);
    assert(!reopened.selection.light_enabled);
    expect_body(&reopened, KB_STORE_BASE, "default", "updated");
    expect_body(&reopened, KB_STORE_MACRO, "macro", "steps");
    expect_body(&reopened, KB_STORE_MACRO, "\xe6\xb5\x8b\xe8\xaf\x95\xe5\xae\x8f", "utf8");
    assert(kb_store_gc(&reopened) == 0);

    /* Root-record corruption must preserve the volume and all user files. */
    fd = nos_open("/data/keyboard/config.nkc", O_WRONLY);
    assert(fd >= 0 && nos_write(fd, "BAD!", 4) == 4);
    assert(nos_fsync(fd) == 0 && nos_close(fd) == 0);
    unmount_volume();
    memcpy(saved_flash, flash, sizeof(flash));
    before_erases = erases;
    mount_volume();
    assert(kb_store_open(&reopened) == -EINVAL && !reopened.ready);
    assert(kb_store_seed(&reopened, "base", 4, "light", 5) == -EEXIST);
    assert(erases == before_erases);
    assert(memcmp(saved_flash, flash, sizeof(flash)) == 0);
    assert(kb_store_read(&store, KB_STORE_MACRO, "macro", KB_STORE_CURRENT_REVISION,
                         0, &byte, 1) == 1 && byte == 's');
    unmount_volume();
    /* Underlying filesystem metadata corruption must not trigger format. */
    memset(flash, 0, 2 * BLOCK_SIZE);
    memcpy(saved_flash, flash, sizeof(flash)); before_erases = erases;
    assert(littlefs_fs_type.mount(&littlefs_fs_type, 0, "keyboard-test", NULL) == NULL);
    assert(erases == before_erases && memcmp(saved_flash, flash, sizeof(flash)) == 0);
    assert(allocations == 0);
    puts("keyboard store real VFS/LittleFS tests passed");
    return 0;
}
