/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS AI-Native 统一零拷贝内存对象 (VMO / TensorObj) 内核接口。
 * 支持不可变权重封印 (Seal)、写时复制 (CoW)、KV 前缀共享、W^X 硬件隔离与可重计算驱逐。
 */

#ifndef __NOS_VMO_H__
#define __NOS_VMO_H__

#include <uapi/nos/types.h>
#include <uapi/nos/ai.h>
#include <kernel/spinlock.h>
#include <nos/refcount.h>
#include <nos/object.h>
#include <nos/mm.h>

struct nos_cell;
struct nos_notification;

/* 物理 4KB 页描述符：通过独立原子引用计数支持多 VMO 间 CoW 与 KV-Cache 前缀零拷贝共享 */
struct nos_vmo_page {
    nos_refcount_t refcount;
    uintptr_t      kvaddr;
    uintptr_t      paddr;
};

struct nos_vmo {
    struct nos_object        header;      /* 必须置于首字段，用于 Capability 查找与引用固定 */
    uint32_t                 kind;        /* enum nos_vmo_kind */
    uint32_t                 flags;       /* NOS_VMO_FLAG_* */
    uint32_t                 block_bytes; /* KV-Cache 分块大小（默认 65536 或 PAGE_SIZE） */
    uint32_t                 page_count;  /* 4KB 逻辑页数量 */
    size_t                   size_bytes;  /* VMO 总大小（页对齐） */
    bool                     sealed;      /* 永久不可变封印标志 */
    spinlock_t               lock;        /* 保护 pages[]、sealed 与 evict_notif */
    struct nos_vmo_page    **pages;       /* 按逻辑页索引的物理页指针数组 */
    struct nos_notification *evict_notif; /* 可重计算页被驱逐时的异步通知对象 */
};

struct nos_vmo *nos_vmo_create(const struct nos_vmo_create_args *args);
int nos_vmo_seal(struct nos_vmo *vmo);
struct nos_vmo *nos_vmo_clone_cow(struct nos_vmo *src);
struct nos_vmo *nos_vmo_clone_kv_prefix(struct nos_vmo *src, size_t prefix_bytes);

int nos_vmo_read(struct nos_vmo *vmo, size_t offset, void *buf, size_t len);
int nos_vmo_write(struct nos_vmo *vmo, size_t offset, const void *buf, size_t len);

int nos_vmo_map(struct nos_vmo *vmo,
                struct nos_vm_space *space,
                uintptr_t vaddr,
                size_t offset,
                size_t len,
                uint32_t prot_flags,
                struct nos_cell *charge_cell);

int nos_vmo_remap_range(struct nos_vmo *vmo,
                        struct nos_vm_space *space,
                        uintptr_t vaddr,
                        size_t offset,
                        size_t len,
                        uint32_t prot_flags);

int nos_vmo_cow_break_page(struct nos_vmo *vmo,
                           uint32_t page_idx,
                           uintptr_t *out_paddr);

int nos_vmo_restore_from_snapshot(struct nos_vmo *dst, struct nos_vmo *snap);

int nos_vmo_unmap(struct nos_vm_space *space,
                  uintptr_t vaddr,
                  size_t len,
                  struct nos_cell *charge_cell);

int nos_vmo_bind_evict_notif(struct nos_vmo *vmo, struct nos_notification *notif);
int nos_vmo_evict_pages(struct nos_vmo *vmo, size_t offset, size_t len);

#endif /* __NOS_VMO_H__ */
