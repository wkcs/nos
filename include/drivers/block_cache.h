/**
 * Copyright (C) 2026 Nick Hu
 *
 * Author: Nick Hu
 */

#ifndef __NOS_DRIVERS_BLOCK_CACHE_H__
#define __NOS_DRIVERS_BLOCK_CACHE_H__

#include <drivers/block.h>
#include <kernel/device.h>
#include <kernel/errno.h>
#include <kernel/mutex.h>
#include <stdbool.h>
#include <stdint.h>

/* 块缓存状态标志 */
#define BC_FLAG_VALID   (1U << 0)   /* 条目有效且持有缓存数据 */
#define BC_FLAG_DIRTY   (1U << 1)   /* 条目已修改，尚未回写到物理介质 */
#define BC_FLAG_LOCKED  (1U << 2)   /* 条目被锁定，禁止被 LRU 淘汰 */

/* 单个扇区缓存条目 */
struct block_cache_entry {
    uint32_t lba;                /* 逻辑块地址 (Logical Block Address) */
    uint32_t flags;              /* 状态标志位 (BC_FLAG_*) */
    uint32_t access_seq;         /* 访问序号，用于 LRU 淘汰置换 */
    uint8_t *data;               /* 扇区数据缓冲区指针 */
};

/* 通用块缓存控制块 */
struct block_cache {
    const struct device *raw_bdev;      /* 底层物理块设备句柄 */
    uint32_t sector_size;               /* 扇区大小（字节，通常为 512） */
    uint16_t entry_count;               /* 缓存条目总数 */
    uint32_t seq_counter;               /* 全局访问递增计数器 */
    bool is_dynamic;                    /* 是否由 block_cache 内部动态分配内存 */
    struct mutex lock;                  /* 互斥锁，保护并发读写与置换 */
    struct block_cache_entry *entries;  /* 缓存条目数组 */
    uint8_t *data_pool;                 /* 扇区数据内存池 */
};

/**
 * 初始化块缓存模块
 *
 * @param bc 块缓存控制块指针
 * @param raw_bdev 底层块设备指针
 * @param entry_count 缓存条目数（例如 4 或 8）
 * @param sector_size 扇区字节数（通常为 512）
 * @return 0 成功，负数表示错误码
 */
int block_cache_init(struct block_cache *bc, const struct device *raw_bdev,
                     uint16_t entry_count, uint32_t sector_size);

/**
 * 从块缓存读取扇区数据
 *
 * @param bc 块缓存控制块指针
 * @param lba 起始逻辑块地址
 * @param buf 目标数据缓冲区
 * @param count 读取扇区数
 * @return 0 成功，负数表示错误码
 */
int block_cache_read(struct block_cache *bc, uint32_t lba, void *buf,
                     uint32_t count);

/**
 * 写入扇区数据至块缓存（写合并与延迟写回）
 *
 * @param bc 块缓存控制块指针
 * @param lba 起始逻辑块地址
 * @param buf 源数据缓冲区
 * @param count 写入扇区数
 * @return 0 成功，负数表示错误码
 */
int block_cache_write(struct block_cache *bc, uint32_t lba, const void *buf,
                      uint32_t count);

/**
 * 将所有脏块按 LBA 升序回写物理介质并清除脏标志
 *
 * @param bc 块缓存控制块指针
 * @return 0 成功，负数表示首次发生的错误码
 */
int block_cache_sync(struct block_cache *bc);

/**
 * 回写指定 LBA 的脏扇区
 *
 * @param bc 块缓存控制块指针
 * @param lba 逻辑块地址
 * @return 0 成功，负数表示错误码
 */
int block_cache_flush_lba(struct block_cache *bc, uint32_t lba);

/**
 * 使指定 LBA 范围的缓存条目失效（放弃数据并不再视为命中）
 *
 * @param bc 块缓存控制块指针
 * @param lba 起始逻辑块地址
 * @param count 扇区数
 * @return 0 成功，负数表示错误码
 */
int block_cache_invalidate(struct block_cache *bc, uint32_t lba, uint32_t count);

/**
 * 擦除指定 LBA 范围并使对应缓存条目失效
 *
 * @param bc 块缓存控制块指针
 * @param lba 起始逻辑块地址
 * @param count 扇区数
 * @return 0 成功，负数表示错误码
 */
int block_cache_erase(struct block_cache *bc, uint32_t lba, uint32_t count);

/**
 * 析构块缓存并释放所占内存资源
 *
 * @param bc 块缓存控制块指针
 */
void block_cache_deinit(struct block_cache *bc);

#endif /* __NOS_DRIVERS_BLOCK_CACHE_H__ */
