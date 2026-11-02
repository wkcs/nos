/**
 * Copyright (C) 2026 Nick Hu
 *
 * Author: Nick Hu
 */

#include <drivers/block_cache.h>
#include <kernel/mm.h>
#include <string.h>

/**
 * 寻找指定 LBA 的缓存条目
 */
static struct block_cache_entry *block_cache_find(struct block_cache *bc, uint32_t lba)
{
    uint16_t i;

    for (i = 0; i < bc->entry_count; i++) {
        if ((bc->entries[i].flags & BC_FLAG_VALID) &&
            bc->entries[i].lba == lba) {
            return &bc->entries[i];
        }
    }
    return NULL;
}

/**
 * 寻找可用于替换置换的缓存槽位（优先未使用的槽位，其次 LRU 未锁定槽位）
 */
static struct block_cache_entry *block_cache_find_victim(struct block_cache *bc)
{
    struct block_cache_entry *victim = NULL;
    uint32_t oldest_seq = UINT32_MAX;
    uint16_t i;

    /* 1. 优先使用无效（未占用）的槽位 */
    for (i = 0; i < bc->entry_count; i++) {
        if (!(bc->entries[i].flags & BC_FLAG_VALID)) {
            return &bc->entries[i];
        }
    }

    /* 2. 槽位均被占用时，在未锁定槽位中寻找最久未访问 (LRU) 的条目 */
    for (i = 0; i < bc->entry_count; i++) {
        if (bc->entries[i].flags & BC_FLAG_LOCKED) {
            continue;
        }
        if (bc->entries[i].access_seq <= oldest_seq) {
            oldest_seq = bc->entries[i].access_seq;
            victim = &bc->entries[i];
        }
    }

    return victim;
}

/**
 * 若条目有效且处于脏状态，回写到底层物理介质并清除脏标志
 */
static int block_cache_flush_entry(struct block_cache *bc, struct block_cache_entry *entry)
{
    if ((entry->flags & (BC_FLAG_VALID | BC_FLAG_DIRTY)) ==
        (BC_FLAG_VALID | BC_FLAG_DIRTY)) {
        int ret = block_write(bc->raw_bdev, entry->lba, entry->data, 1);
        if (ret != 0) {
            return ret;
        }
        entry->flags &= ~BC_FLAG_DIRTY;
    }
    return 0;
}

int block_cache_init(struct block_cache *bc, const struct device *raw_bdev,
                     uint16_t entry_count, uint32_t sector_size)
{
    size_t entries_sz;
    size_t pool_sz;
    uint16_t i;

    if (bc == NULL || raw_bdev == NULL || entry_count == 0 || sector_size == 0) {
        return -EINVAL;
    }

    mutex_init(&bc->lock);

    entries_sz = sizeof(struct block_cache_entry) * entry_count;
    pool_sz = (size_t)sector_size * entry_count;

    if (bc->entries == NULL) {
        /* 单次分配 entries 与 data_pool 紧凑内存，降低堆碎片与调用开销 */
        bc->entries = kzalloc(entries_sz + pool_sz, GFP_KERNEL);
        if (bc->entries == NULL) {
            return -ENOMEM;
        }
        bc->data_pool = (uint8_t *)(bc->entries + entry_count);
    } else if (bc->data_pool == NULL) {
        bc->data_pool = kzalloc(pool_sz, GFP_KERNEL);
        if (bc->data_pool == NULL) {
            return -ENOMEM;
        }
    }

    for (i = 0; i < entry_count; i++) {
        bc->entries[i].data = bc->data_pool + (size_t)i * sector_size;
        bc->entries[i].flags = 0;
        bc->entries[i].lba = 0;
        bc->entries[i].access_seq = 0;
    }

    bc->raw_bdev = raw_bdev;
    bc->sector_size = sector_size;
    bc->entry_count = entry_count;
    bc->seq_counter = 0;

    return 0;
}

int block_cache_read(struct block_cache *bc, uint32_t lba, void *buf,
                     uint32_t count)
{
    uint32_t i;
    int ret = 0;

    if (bc == NULL || bc->raw_bdev == NULL || buf == NULL || count == 0) {
        return -EINVAL;
    }

    mutex_lock(&bc->lock);

    for (i = 0; i < count; i++) {
        uint32_t cur_lba = lba + i;
        uint8_t *cur_buf = (uint8_t *)buf + (size_t)i * bc->sector_size;
        struct block_cache_entry *entry = block_cache_find(bc, cur_lba);

        if (entry != NULL) {
            /* 命中：直接从 RAM 复制并更新访问序号 */
            memcpy(cur_buf, entry->data, bc->sector_size);
            entry->access_seq = ++bc->seq_counter;
            continue;
        }

        /* 未命中：寻找置换槽位并回写脏数据 */
        entry = block_cache_find_victim(bc);
        if (entry == NULL) {
            ret = -EBUSY;
            break;
        }

        ret = block_cache_flush_entry(bc, entry);
        if (ret != 0) {
            break;
        }

        /* 从物理介质读入扇区数据 */
        ret = block_read(bc->raw_bdev, cur_lba, entry->data, 1);
        if (ret != 0) {
            entry->flags &= ~BC_FLAG_VALID;
            break;
        }

        entry->lba = cur_lba;
        entry->flags = BC_FLAG_VALID;
        entry->access_seq = ++bc->seq_counter;
        memcpy(cur_buf, entry->data, bc->sector_size);
    }

    mutex_unlock(&bc->lock);
    return ret;
}

int block_cache_write(struct block_cache *bc, uint32_t lba, const void *buf,
                      uint32_t count)
{
    uint32_t i;
    int ret = 0;

    if (bc == NULL || bc->raw_bdev == NULL || buf == NULL || count == 0) {
        return -EINVAL;
    }

    mutex_lock(&bc->lock);

    for (i = 0; i < count; i++) {
        uint32_t cur_lba = lba + i;
        const uint8_t *cur_buf = (const uint8_t *)buf + (size_t)i * bc->sector_size;
        struct block_cache_entry *entry = block_cache_find(bc, cur_lba);

        if (entry != NULL) {
            /* 命中：覆写 RAM 缓存，标记 DIRTY，不立即触发 Flash 擦写 */
            memcpy(entry->data, cur_buf, bc->sector_size);
            entry->flags |= BC_FLAG_DIRTY;
            entry->access_seq = ++bc->seq_counter;
            continue;
        }

        /* 未命中：寻找置换槽位并回写脏数据 */
        entry = block_cache_find_victim(bc);
        if (entry == NULL) {
            ret = -EBUSY;
            break;
        }

        ret = block_cache_flush_entry(bc, entry);
        if (ret != 0) {
            break;
        }

        /* 覆写新槽位数据并置位 DIRTY */
        memcpy(entry->data, cur_buf, bc->sector_size);
        entry->lba = cur_lba;
        entry->flags = BC_FLAG_VALID | BC_FLAG_DIRTY;
        entry->access_seq = ++bc->seq_counter;
    }

    mutex_unlock(&bc->lock);
    return ret;
}

int block_cache_sync(struct block_cache *bc)
{
    struct block_cache_entry *dirty_list[64];
    uint16_t dirty_count = 0;
    uint16_t i, j;
    int first_err = 0;

    if (bc == NULL || bc->raw_bdev == NULL) {
        return -EINVAL;
    }

    mutex_lock(&bc->lock);

    if (bc->entries == NULL || bc->entry_count == 0) {
        mutex_unlock(&bc->lock);
        return 0;
    }

    /* 收集所有处于有效且脏状态的条目 */
    for (i = 0; i < bc->entry_count; i++) {
        struct block_cache_entry *e = &bc->entries[i];
        if ((e->flags & (BC_FLAG_VALID | BC_FLAG_DIRTY)) ==
            (BC_FLAG_VALID | BC_FLAG_DIRTY)) {
            if (dirty_count < (uint16_t)(sizeof(dirty_list) / sizeof(dirty_list[0]))) {
                dirty_list[dirty_count++] = e;
            } else {
                int ret = block_cache_flush_entry(bc, e);
                if (ret != 0 && first_err == 0) {
                    first_err = ret;
                }
            }
        }
    }

    /* 按 LBA 升序排序收集到的脏块，保证向物理介质顺序写入 */
    for (i = 0; i < dirty_count; i++) {
        for (j = i + 1; j < dirty_count; j++) {
            if (dirty_list[j]->lba < dirty_list[i]->lba) {
                struct block_cache_entry *tmp = dirty_list[i];
                dirty_list[i] = dirty_list[j];
                dirty_list[j] = tmp;
            }
        }
    }

    /* 顺序回写物理存储介质 */
    for (i = 0; i < dirty_count; i++) {
        int ret = block_cache_flush_entry(bc, dirty_list[i]);
        if (ret != 0 && first_err == 0) {
            first_err = ret;
        }
    }

    mutex_unlock(&bc->lock);
    return first_err;
}

int block_cache_flush_lba(struct block_cache *bc, uint32_t lba)
{
    struct block_cache_entry *e;
    int ret = 0;

    if (bc == NULL || bc->raw_bdev == NULL) {
        return -EINVAL;
    }

    mutex_lock(&bc->lock);
    e = block_cache_find(bc, lba);
    if (e != NULL) {
        ret = block_cache_flush_entry(bc, e);
    }
    mutex_unlock(&bc->lock);
    return ret;
}

void block_cache_deinit(struct block_cache *bc)
{
    if (bc == NULL) {
        return;
    }

    /* 析构前刷盘同步所有脏块 */
    (void)block_cache_sync(bc);

    mutex_lock(&bc->lock);
    if (bc->entries != NULL) {
        if (bc->data_pool != NULL &&
            bc->data_pool != (uint8_t *)(bc->entries + bc->entry_count)) {
            kfree(bc->data_pool);
        }
        kfree(bc->entries);
        bc->entries = NULL;
        bc->data_pool = NULL;
    } else if (bc->data_pool != NULL) {
        kfree(bc->data_pool);
        bc->data_pool = NULL;
    }

    bc->entry_count = 0;
    bc->sector_size = 0;
    bc->seq_counter = 0;
    bc->raw_bdev = NULL;
    mutex_unlock(&bc->lock);
}
