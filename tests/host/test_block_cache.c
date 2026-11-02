/**
 * Copyright (C) 2026 Nick Hu
 *
 * Block Cache host regression test.
 */

#include <assert.h>
#include <kernel/kernel.h>
#include <drivers/block.h>
#include <drivers/block_cache.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Host stubs for mm */
void *kzalloc(size_t size, gfp_t flags)
{
    (void)flags;
    return calloc(1, size);
}

void kfree(void *ptr)
{
    free(ptr);
}

#define MOCK_DISK_SECTORS 32
#define MOCK_SECTOR_SIZE  512

static uint8_t g_mock_disk[MOCK_DISK_SECTORS][MOCK_SECTOR_SIZE];
static int g_read_calls = 0;
static int g_write_calls = 0;
static uint32_t g_write_history[64];
static int g_write_history_count = 0;

static int mock_read(const struct device *dev, uint32_t lba, void *buf, uint32_t count)
{
    (void)dev;
    if (lba + count > MOCK_DISK_SECTORS) {
        return -EINVAL;
    }
    for (uint32_t i = 0; i < count; i++) {
        memcpy((uint8_t *)buf + i * MOCK_SECTOR_SIZE, g_mock_disk[lba + i], MOCK_SECTOR_SIZE);
        g_read_calls++;
    }
    return 0;
}

static int mock_write(const struct device *dev, uint32_t lba, const void *buf, uint32_t count)
{
    (void)dev;
    if (lba + count > MOCK_DISK_SECTORS) {
        return -EINVAL;
    }
    for (uint32_t i = 0; i < count; i++) {
        memcpy(g_mock_disk[lba + i], (const uint8_t *)buf + i * MOCK_SECTOR_SIZE, MOCK_SECTOR_SIZE);
        if (g_write_history_count < 64) {
            g_write_history[g_write_history_count++] = lba + i;
        }
        g_write_calls++;
    }
    return 0;
}

static int mock_get_info(const struct device *dev, uint32_t *sector_size, uint32_t *sector_count)
{
    (void)dev;
    *sector_size = MOCK_SECTOR_SIZE;
    *sector_count = MOCK_DISK_SECTORS;
    return 0;
}

static struct block_driver_api g_mock_api = {
    .read = mock_read,
    .write = mock_write,
    .get_info = mock_get_info,
};

static struct device g_mock_dev = {
    .api = &g_mock_api,
    .ready = true,
};

static void reset_mock_state(void)
{
    g_read_calls = 0;
    g_write_calls = 0;
    g_write_history_count = 0;
    for (uint32_t i = 0; i < MOCK_DISK_SECTORS; i++) {
        memset(g_mock_disk[i], (uint8_t)(0xA0 + i), MOCK_SECTOR_SIZE);
    }
}

static void test_param_validation(void)
{
    struct block_cache bc;

    assert(block_cache_init(NULL, &g_mock_dev, 4, 512) == -EINVAL);
    assert(block_cache_init(&bc, NULL, 4, 512) == -EINVAL);
    assert(block_cache_init(&bc, &g_mock_dev, 0, 512) == -EINVAL);
    assert(block_cache_init(&bc, &g_mock_dev, 4, 0) == -EINVAL);
    assert(block_cache_sync(NULL) == -EINVAL);
    assert(block_cache_flush_lba(NULL, 0) == -EINVAL);
}

static void test_read_hit_and_miss(void)
{
    struct block_cache bc;
    uint8_t buf[MOCK_SECTOR_SIZE];

    reset_mock_state();
    memset(&bc, 0, sizeof(bc));
    assert(block_cache_init(&bc, &g_mock_dev, 4, MOCK_SECTOR_SIZE) == 0);

    /* 首次读取 LBA 1: 发生未命中，从底盘读取 */
    assert(block_cache_read(&bc, 1, buf, 1) == 0);
    assert(buf[0] == (uint8_t)(0xA0 + 1));
    assert(g_read_calls == 1);

    /* 再次读取 LBA 1: 发生命中，不产生底层读调用 */
    memset(buf, 0, sizeof(buf));
    assert(block_cache_read(&bc, 1, buf, 1) == 0);
    assert(buf[0] == (uint8_t)(0xA0 + 1));
    assert(g_read_calls == 1);

    block_cache_deinit(&bc);
}

static void test_write_merge_and_sync(void)
{
    struct block_cache bc;
    uint8_t wbuf[MOCK_SECTOR_SIZE];
    uint8_t rbuf[MOCK_SECTOR_SIZE];

    reset_mock_state();
    memset(&bc, 0, sizeof(bc));
    assert(block_cache_init(&bc, &g_mock_dev, 4, MOCK_SECTOR_SIZE) == 0);

    /* 写入 LBA 2：更新 RAM 缓存，标记 DIRTY，不立即写底盘 */
    memset(wbuf, 0x55, sizeof(wbuf));
    assert(block_cache_write(&bc, 2, wbuf, 1) == 0);
    assert(g_write_calls == 0);

    /* 再次修改 LBA 2（写合并）：依然只在 RAM 更新 */
    memset(wbuf, 0x77, sizeof(wbuf));
    assert(block_cache_write(&bc, 2, wbuf, 1) == 0);
    assert(g_write_calls == 0);

    /* 读取 LBA 2：从缓存命中获取最新 0x77 数据 */
    assert(block_cache_read(&bc, 2, rbuf, 1) == 0);
    assert(rbuf[0] == 0x77);
    assert(g_read_calls == 0);

    /* 执行 block_cache_sync：触发回写 */
    assert(block_cache_sync(&bc) == 0);
    assert(g_write_calls == 1);
    assert(g_mock_disk[2][0] == 0x77);

    /* 再次 sync：脏标志已清，无新增写 */
    assert(block_cache_sync(&bc) == 0);
    assert(g_write_calls == 1);

    block_cache_deinit(&bc);
}

static void test_sync_ordering(void)
{
    struct block_cache bc;
    uint8_t wbuf[MOCK_SECTOR_SIZE];

    reset_mock_state();
    memset(&bc, 0, sizeof(bc));
    assert(block_cache_init(&bc, &g_mock_dev, 8, MOCK_SECTOR_SIZE) == 0);

    /* 乱序写入 LBA 5, 2, 7, 1 */
    memset(wbuf, 0x11, sizeof(wbuf));
    assert(block_cache_write(&bc, 5, wbuf, 1) == 0);
    assert(block_cache_write(&bc, 2, wbuf, 1) == 0);
    assert(block_cache_write(&bc, 7, wbuf, 1) == 0);
    assert(block_cache_write(&bc, 1, wbuf, 1) == 0);
    assert(g_write_calls == 0);

    /* 触发 sync，回写顺序必须为 LBA 升序：1, 2, 5, 7 */
    assert(block_cache_sync(&bc) == 0);
    assert(g_write_calls == 4);
    assert(g_write_history_count == 4);
    assert(g_write_history[0] == 1);
    assert(g_write_history[1] == 2);
    assert(g_write_history[2] == 5);
    assert(g_write_history[3] == 7);

    block_cache_deinit(&bc);
}

static void test_lru_eviction(void)
{
    struct block_cache bc;
    uint8_t buf[MOCK_SECTOR_SIZE];

    reset_mock_state();
    memset(&bc, 0, sizeof(bc));
    /* 容量仅为 2 个条目 */
    assert(block_cache_init(&bc, &g_mock_dev, 2, MOCK_SECTOR_SIZE) == 0);

    /* 填满缓存：LBA 0 与 LBA 1 */
    assert(block_cache_read(&bc, 0, buf, 1) == 0);
    assert(block_cache_read(&bc, 1, buf, 1) == 0);
    assert(g_read_calls == 2);

    /* 访问 LBA 0，使其比 LBA 1 更新 */
    assert(block_cache_read(&bc, 0, buf, 1) == 0);
    assert(g_read_calls == 2);

    /* 读取 LBA 2：应淘汰较旧的 LBA 1，保留 LBA 0 */
    assert(block_cache_read(&bc, 2, buf, 1) == 0);
    assert(g_read_calls == 3);

    /* 再次读取 LBA 0：应仍然命中，不触发读底盘 */
    assert(block_cache_read(&bc, 0, buf, 1) == 0);
    assert(g_read_calls == 3);

    /* 再次读取 LBA 1：之前被淘汰，此时未命中产生第 4 次底层读 */
    assert(block_cache_read(&bc, 1, buf, 1) == 0);
    assert(g_read_calls == 4);

    block_cache_deinit(&bc);
}

static void test_dirty_eviction(void)
{
    struct block_cache bc;
    uint8_t wbuf[MOCK_SECTOR_SIZE];
    uint8_t rbuf[MOCK_SECTOR_SIZE];

    reset_mock_state();
    memset(&bc, 0, sizeof(bc));
    /* 容量 2 个条目 */
    assert(block_cache_init(&bc, &g_mock_dev, 2, MOCK_SECTOR_SIZE) == 0);

    /* 写入脏块 LBA 0 与 LBA 1 */
    memset(wbuf, 0x88, sizeof(wbuf));
    assert(block_cache_write(&bc, 0, wbuf, 1) == 0);
    memset(wbuf, 0x99, sizeof(wbuf));
    assert(block_cache_write(&bc, 1, wbuf, 1) == 0);
    assert(g_write_calls == 0);

    /* 读入 LBA 2，必须淘汰最老的 LBA 0，并在置换前将其回写 */
    assert(block_cache_read(&bc, 2, rbuf, 1) == 0);
    assert(g_write_calls == 1);
    assert(g_write_history[0] == 0);
    assert(g_mock_disk[0][0] == 0x88);

    block_cache_deinit(&bc);
}

static void test_flush_lba(void)
{
    struct block_cache bc;
    uint8_t wbuf[MOCK_SECTOR_SIZE];

    reset_mock_state();
    memset(&bc, 0, sizeof(bc));
    assert(block_cache_init(&bc, &g_mock_dev, 4, MOCK_SECTOR_SIZE) == 0);

    memset(wbuf, 0x33, sizeof(wbuf));
    assert(block_cache_write(&bc, 4, wbuf, 1) == 0);
    assert(block_cache_write(&bc, 5, wbuf, 1) == 0);
    assert(g_write_calls == 0);

    /* 单独回写 LBA 4 */
    assert(block_cache_flush_lba(&bc, 4) == 0);
    assert(g_write_calls == 1);
    assert(g_mock_disk[4][0] == 0x33);

    /* 再次单独回写 LBA 4：已干净，不重复写 */
    assert(block_cache_flush_lba(&bc, 4) == 0);
    assert(g_write_calls == 1);

    block_cache_deinit(&bc);
}

static void test_multi_sector(void)
{
    struct block_cache bc;
    uint8_t wbuf[MOCK_SECTOR_SIZE * 3];
    uint8_t rbuf[MOCK_SECTOR_SIZE * 3];

    reset_mock_state();
    memset(&bc, 0, sizeof(bc));
    assert(block_cache_init(&bc, &g_mock_dev, 8, MOCK_SECTOR_SIZE) == 0);

    for (int i = 0; i < 3; i++) {
        memset(wbuf + i * MOCK_SECTOR_SIZE, (uint8_t)(0xC0 + i), MOCK_SECTOR_SIZE);
    }
    assert(block_cache_write(&bc, 10, wbuf, 3) == 0);
    assert(g_write_calls == 0);

    assert(block_cache_read(&bc, 10, rbuf, 3) == 0);
    assert(rbuf[0] == 0xC0);
    assert(rbuf[MOCK_SECTOR_SIZE] == 0xC1);
    assert(rbuf[MOCK_SECTOR_SIZE * 2] == 0xC2);

    assert(block_cache_sync(&bc) == 0);
    assert(g_write_calls == 3);
    assert(g_mock_disk[10][0] == 0xC0);
    assert(g_mock_disk[11][0] == 0xC1);
    assert(g_mock_disk[12][0] == 0xC2);

    block_cache_deinit(&bc);
}

int main(void)
{
    test_param_validation();
    test_read_hit_and_miss();
    test_write_merge_and_sync();
    test_sync_ordering();
    test_lru_eviction();
    test_dirty_eviction();
    test_flush_lba();
    test_multi_sector();

    puts("block cache checks passed");
    return 0;
}
