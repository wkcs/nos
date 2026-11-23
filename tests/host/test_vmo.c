/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * 宿主端 TDD 测试：VMO / TensorObj 统一零拷贝内存对象
 * 覆盖：创建、Sealed 不可变权重、W^X 硬件映射校验、CoW 写时复制、KV 前缀共享、可重计算驱逐与配额回滚。
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uapi/nos/errno.h>
#include <uapi/nos/ai.h>
#include <nos/vmo.h>
#include <nos/cell.h>
#include <nos/notification.h>

uintptr_t kernel_running = 1;

void *kmalloc(size_t size, gfp_t flags)
{
    (void)flags;
    return malloc(size);
}

void *kzalloc(size_t size, gfp_t flags)
{
    (void)flags;
    return calloc(1, size);
}

void kfree(void *ptr)
{
    free(ptr);
}

/* Mock HAL 虚拟页表映射与配额/通知记录器 */
struct mock_map_record {
    uintptr_t vaddr;
    uintptr_t paddr;
    size_t    size;
    uint32_t  prot;
    bool      active;
};

static struct mock_map_record g_maps[32];
static uint32_t g_map_call_count;
static uint32_t g_unmap_call_count;
static int32_t  g_fail_map_on_call = -1;
static uint64_t g_mock_cell_mem_used;
static uint64_t g_mock_cell_mem_limit = UINT64_MAX;
static uint64_t g_mock_evict_signals;
static uint32_t g_mock_dcache_ops[4];
static uint32_t g_mock_af_bits;

void hal_dcache_sync(uintptr_t va, size_t size, uint32_t dir)
{
    (void)va;
    if (size > 0U && dir <= NOS_DMA_BIDIRECTIONAL) {
        g_mock_dcache_ops[dir]++;
    }
}

int hal_vm_test_and_clear_af(struct nos_vm_space *space,
                             uintptr_t vaddr,
                             bool *was_accessed)
{
    uint32_t i;
    (void)space;
    if (was_accessed == NULL) {
        return -EINVAL;
    }
    for (i = 0U; i < 32U; i++) {
        if (g_maps[i].active &&
            vaddr >= g_maps[i].vaddr &&
            vaddr < (g_maps[i].vaddr + g_maps[i].size)) {
            uint32_t bit = (uint32_t)((vaddr - g_maps[i].vaddr) / 4096U);
            bool af = (bit < 32U) ? ((g_mock_af_bits & (1U << bit)) != 0U) : false;
            *was_accessed = af;
            if (af && bit < 32U) {
                g_mock_af_bits &= ~(1U << bit);
            }
            return 0;
        }
    }
    return -ENOENT;
}

int hal_vm_map(struct nos_vm_space *space,
               uintptr_t vaddr,
               uintptr_t paddr,
               size_t size,
               uint32_t prot_flags)
{
    uint32_t i;
    (void)space;

    g_map_call_count++;
    if (g_fail_map_on_call > 0 && (int32_t)g_map_call_count == g_fail_map_on_call) {
        return -EIO;
    }

    for (i = 0U; i < 32U; i++) {
        if (!g_maps[i].active) {
            g_maps[i].vaddr = vaddr;
            g_maps[i].paddr = paddr;
            g_maps[i].size = size;
            g_maps[i].prot = prot_flags;
            g_maps[i].active = true;
            return 0;
        }
    }
    return -ENOMEM;
}

int hal_vm_unmap(struct nos_vm_space *space, uintptr_t vaddr, size_t size)
{
    uint32_t i;
    (void)space;

    g_unmap_call_count++;
    for (i = 0U; i < 32U; i++) {
        if (g_maps[i].active &&
            g_maps[i].vaddr >= vaddr &&
            (g_maps[i].vaddr + g_maps[i].size) <= (vaddr + size)) {
            g_maps[i].active = false;
        }
    }
    return 0;
}

int nos_cell_charge_mem(struct nos_cell *cell, uint64_t bytes)
{
    (void)cell;
    if (g_mock_cell_mem_used + bytes > g_mock_cell_mem_limit) {
        return -EDQUOT;
    }
    g_mock_cell_mem_used += bytes;
    return 0;
}

void nos_cell_uncharge_mem(struct nos_cell *cell, uint64_t bytes)
{
    (void)cell;
    if (g_mock_cell_mem_used >= bytes) {
        g_mock_cell_mem_used -= bytes;
    } else {
        g_mock_cell_mem_used = 0ULL;
    }
}

struct nos_vm_space *nos_cell_vm_space(struct nos_cell *cell)
{
    (void)cell;
    return NULL;
}

int nos_cell_track_vmo_map(struct nos_cell *cell,
                           struct nos_vmo *vmo,
                           uintptr_t vaddr,
                           size_t offset,
                           size_t len,
                           uint32_t prot_flags)
{
    (void)cell; (void)vmo; (void)vaddr; (void)offset; (void)len; (void)prot_flags;
    return 0;
}

void nos_cell_untrack_vmo_map(struct nos_cell *cell, uintptr_t vaddr, size_t len)
{
    (void)cell; (void)vaddr; (void)len;
}

int nos_notify_signal_obj(struct nos_notification *notif, uint64_t bits)
{
    (void)notif;
    g_mock_evict_signals += bits;
    return 0;
}

static void reset_mock_env(void)
{
    memset(g_maps, 0, sizeof(g_maps));
    g_map_call_count = 0U;
    g_unmap_call_count = 0U;
    g_fail_map_on_call = -1;
    g_mock_cell_mem_used = 0ULL;
    g_mock_cell_mem_limit = UINT64_MAX;
    g_mock_evict_signals = 0ULL;
    memset(g_mock_dcache_ops, 0, sizeof(g_mock_dcache_ops));
    g_mock_af_bits = 0U;
}

static void release_vmo(struct nos_vmo *vmo)
{
    assert(nos_object_close(&vmo->header) == 0);
    assert(nos_object_retire(&vmo->header) == 0);
}

static void test_vmo_seal_and_wx(void)
{
    struct nos_vmo_create_args args = {
        .size_bytes = 3U * 4096U,
        .kind = NOS_VMO_WEIGHT,
        .flags = 0U,
        .block_bytes = 0U,
    };
    struct nos_vm_space dummy_space;
    struct nos_vmo *vmo;
    uint8_t write_buf[64];
    uint8_t read_buf[64];
    uint32_t i;

    reset_mock_env();
    memset(&dummy_space, 0, sizeof(dummy_space));

    /* 非法对齐与零大小应拒绝 */
    struct nos_vmo_create_args bad_args = args;
    bad_args.size_bytes = 4095U;
    assert(nos_vmo_create(&bad_args) == NULL);
    bad_args.size_bytes = 0U;
    assert(nos_vmo_create(&bad_args) == NULL);

    vmo = nos_vmo_create(&args);
    assert(vmo != NULL);
    assert(vmo->page_count == 3U);

    for (i = 0U; i < sizeof(write_buf); i++) {
        write_buf[i] = (uint8_t)(0xA0U + i);
    }
    /* 跨页写入并读回校验 */
    assert(nos_vmo_write(vmo, 4080U, write_buf, sizeof(write_buf)) == 0);
    memset(read_buf, 0, sizeof(read_buf));
    assert(nos_vmo_read(vmo, 4080U, read_buf, sizeof(read_buf)) == 0);
    assert(memcmp(write_buf, read_buf, sizeof(write_buf)) == 0);

    /* W^X 强拦截 */
    assert(nos_vmo_map(vmo, &dummy_space, 0x10000000UL, 0U, 4096U,
                       HAL_VM_PROT_USER | HAL_VM_PROT_WRITE | HAL_VM_PROT_EXEC,
                       NULL) == -EPERM);

    /* 永久封印后禁止写入与可写映射，但允许只读共享映射 */
    assert(nos_vmo_seal(vmo) == 0);
    assert(nos_vmo_seal(vmo) == 0);
    assert(nos_vmo_write(vmo, 0U, write_buf, 8U) == -EACCES);
    assert(nos_vmo_map(vmo, &dummy_space, 0x10000000UL, 0U, 4096U,
                       HAL_VM_PROT_USER | HAL_VM_PROT_READ | HAL_VM_PROT_WRITE,
                       NULL) == -EACCES);
    assert(nos_vmo_map(vmo, &dummy_space, 0x10000000UL, 0U, 3U * 4096U,
                       HAL_VM_PROT_USER | HAL_VM_PROT_READ,
                       NULL) == 0);
    assert(nos_vmo_unmap(&dummy_space, 0x10000000UL, 3U * 4096U, NULL) == 0);

    release_vmo(vmo);
}

static void test_vmo_cow_and_kv_prefix(void)
{
    struct nos_vmo_create_args args = {
        .size_bytes = 4U * 4096U,
        .kind = NOS_VMO_KVCACHE,
        .flags = NOS_VMO_FLAG_RECOMPUTABLE,
        .block_bytes = 0U,
    };
    struct nos_vmo *parent;
    struct nos_vmo *cow;
    struct nos_vmo *kv_child;
    struct nos_notification mock_notif;
    uintptr_t orig_pa[4];
    uint8_t msg_parent[16] = "PROMPT_PREFIX_0";
    uint8_t msg_cow[16]    = "COW_BRANCH_PAGE";
    uint8_t msg_tail[16]   = "SUFFIX_GEN_TOK1";
    uint8_t read_back[16];
    uint32_t i;

    reset_mock_env();
    memset(&mock_notif, 0, sizeof(mock_notif));
    (void)nos_object_init(&mock_notif.object, true);

    parent = nos_vmo_create(&args);
    assert(parent != NULL);
    assert(parent->block_bytes == 65536U);

    for (i = 0U; i < 4U; i++) {
        orig_pa[i] = parent->pages[i]->paddr;
        assert((orig_pa[i] & 0xFFFUL) == 0UL);
    }

    assert(nos_vmo_write(parent, 0U, msg_parent, sizeof(msg_parent)) == 0);
    assert(nos_vmo_write(parent, 4096U, msg_parent, sizeof(msg_parent)) == 0);

    /* 1. CoW 克隆应零拷贝共享全部物理页 (refcount == 2) */
    cow = nos_vmo_clone_cow(parent);
    assert(cow != NULL);
    for (i = 0U; i < 4U; i++) {
        assert(cow->pages[i] == parent->pages[i]);
        assert(nos_refcount_read(&parent->pages[i]->refcount) == 2U);
    }

    /* 写 CoW 第 1 页，仅第 1 页发生按页写时复制断裂，其余页保持共享 */
    assert(nos_vmo_write(cow, 4096U, msg_cow, sizeof(msg_cow)) == 0);
    assert(cow->pages[0] == parent->pages[0]);
    assert(cow->pages[1] != parent->pages[1]);
    assert(cow->pages[1]->paddr != orig_pa[1]);
    assert(nos_refcount_read(&parent->pages[1]->refcount) == 1U);
    assert(nos_refcount_read(&cow->pages[1]->refcount) == 1U);

    assert(nos_vmo_read(parent, 4096U, read_back, sizeof(read_back)) == 0);
    assert(memcmp(read_back, msg_parent, sizeof(msg_parent)) == 0);
    assert(nos_vmo_read(cow, 4096U, read_back, sizeof(read_back)) == 0);
    assert(memcmp(read_back, msg_cow, sizeof(msg_cow)) == 0);

    /* 2. KV 前缀共享克隆：前 2 页共享，后 2 页独立私有新页 */
    kv_child = nos_vmo_clone_kv_prefix(parent, 2U * 4096U);
    assert(kv_child != NULL);
    assert(kv_child->pages[0] == parent->pages[0]);
    assert(kv_child->pages[1] == parent->pages[1]);
    assert(nos_refcount_read(&parent->pages[0]->refcount) == 3U);
    assert(nos_refcount_read(&parent->pages[1]->refcount) == 2U);
    assert(kv_child->pages[2] != parent->pages[2]);
    assert(kv_child->pages[3] != parent->pages[3]);

    assert(nos_vmo_write(kv_child, 2U * 4096U, msg_tail, sizeof(msg_tail)) == 0);
    assert(nos_vmo_read(kv_child, 2U * 4096U, read_back, sizeof(read_back)) == 0);
    assert(memcmp(read_back, msg_tail, sizeof(msg_tail)) == 0);
    assert(nos_vmo_read(parent, 2U * 4096U, read_back, sizeof(read_back)) == 0);
    assert(read_back[0] == 0U);

    /* 3. 可重计算页协同驱逐与事件通知 */
    assert(nos_vmo_bind_evict_notif(kv_child, &mock_notif) == 0);
    assert(nos_vmo_evict_pages(kv_child, 2U * 4096U, 4096U) == 0);
    assert(g_mock_evict_signals == 1ULL);
    assert(nos_vmo_read(kv_child, 2U * 4096U, read_back, 4U) == -ENOENT);
    /* 被驱逐页重新写入应自动重分配新页恢复 */
    assert(nos_vmo_write(kv_child, 2U * 4096U, msg_tail, sizeof(msg_tail)) == 0);
    assert(nos_vmo_read(kv_child, 2U * 4096U, read_back, sizeof(msg_tail)) == 0);
    assert(memcmp(read_back, msg_tail, sizeof(msg_tail)) == 0);

    /* 释放全部 VMO 后，共享页计数归零安全释放 */
    release_vmo(kv_child);
    release_vmo(cow);
    release_vmo(parent);
}

static void test_vmo_quota_and_rollback(void)
{
    struct nos_vmo_create_args args = {
        .size_bytes = 3U * 4096U,
        .kind = NOS_VMO_ANON,
        .flags = 0U,
    };
    struct nos_vm_space dummy_space;
    struct nos_cell *fake_cell = (struct nos_cell *)0x1234UL;
    struct nos_vmo *vmo;

    reset_mock_env();
    memset(&dummy_space, 0, sizeof(dummy_space));

    vmo = nos_vmo_create(&args);
    assert(vmo != NULL);

    /* 人为将第 0 页和第 1 页置为独立离散物理地址，确保触发多次 hal_vm_map 并验证第 2 次失败精确回滚 */
    g_mock_cell_mem_limit = 16384ULL;
    assert(nos_vmo_map(vmo, &dummy_space, 0x20000000UL, 0U, 2U * 4096U,
                       HAL_VM_PROT_USER | HAL_VM_PROT_READ, fake_cell) == 0);
    assert(g_mock_cell_mem_used == 8192ULL);

    /* 超配额拒绝映射并保持已记账额度不变 */
    assert(nos_vmo_map(vmo, &dummy_space, 0x30000000UL, 0U, 3U * 4096U,
                       HAL_VM_PROT_USER | HAL_VM_PROT_READ, fake_cell) == -EDQUOT);
    assert(g_mock_cell_mem_used == 8192ULL);

    assert(nos_vmo_unmap(&dummy_space, 0x20000000UL, 2U * 4096U, fake_cell) == 0);
    assert(g_mock_cell_mem_used == 0ULL);

    /* 触发 hal_vm_map 中途失败，核实先退 unmap 再退还 Cell 内存配额 */
    g_map_call_count = 0U;
    g_unmap_call_count = 0U;
    g_fail_map_on_call = 1;
    assert(nos_vmo_map(vmo, &dummy_space, 0x20000000UL, 0U, 3U * 4096U,
                       HAL_VM_PROT_USER | HAL_VM_PROT_READ, fake_cell) == -EIO);
    assert(g_mock_cell_mem_used == 0ULL);

    /* 不可重计算 ANON VMO 拒绝驱逐 */
    assert(nos_vmo_evict_pages(vmo, 0U, 4096U) == -EPERM);

    /* 验证 nos_vmo_cow_break_page 与 nos_vmo_restore_from_snapshot */
    {
        struct nos_vmo *snap;
        uintptr_t orig_pa = vmo->pages[0]->paddr;
        uintptr_t split_pa = 0U;
        char orig_msg[] = "CHKPT_V1";
        char dirt_msg[] = "DIRTY_V2";
        char rb[16] = { 0 };

        assert(nos_vmo_write(vmo, 0U, orig_msg, sizeof(orig_msg)) == 0);
        snap = nos_vmo_clone_cow(vmo);
        assert(snap != NULL);
        assert(nos_vmo_seal(snap) == 0);
        assert(nos_vmo_cow_break_page(vmo, 0U, &split_pa) == 0);
        assert(split_pa != orig_pa && vmo->pages[0]->paddr == split_pa);
        assert(nos_vmo_write(vmo, 0U, dirt_msg, sizeof(dirt_msg)) == 0);
        assert(nos_vmo_restore_from_snapshot(vmo, snap) == 0);
        assert(vmo->pages[0]->paddr == orig_pa);
        assert(nos_vmo_read(vmo, 0U, rb, sizeof(orig_msg)) == 0);
        assert(strcmp(rb, orig_msg) == 0);
        release_vmo(snap);
    }

    release_vmo(vmo);
}

static void test_vmo_stage6_blocks_lazy_hash_sync_and_af_reclaim(void)
{
    struct nos_vmo_create_args c64_args = {
        .size_bytes = 16U * 4096U,
        .kind = NOS_VMO_WEIGHT,
        .flags = NOS_VMO_FLAG_CONTIG_64K,
    };
    struct nos_vmo_create_args lazy_args = {
        .size_bytes = 4U * 4096U,
        .kind = NOS_VMO_ANON,
        .flags = NOS_VMO_FLAG_LAZY_ALLOC | NOS_VMO_FLAG_RECOMPUTABLE,
    };
    struct nos_vmo_create_args dma_args = {
        .size_bytes = 2U * 4096U,
        .kind = NOS_VMO_DMA_TENSOR,
        .flags = 0U,
    };
    struct nos_vm_space dummy_space;
    struct nos_vmo *c64;
    struct nos_vmo *c64_cow;
    struct nos_vmo *lazy;
    struct nos_vmo *dma;
    struct nos_notification ev_notif;
    uint64_t hash1;
    uintptr_t ensure_pa = 0U;
    bool newly = false;
    uint8_t rb[16] = {0};

    reset_mock_env();
    memset(&dummy_space, 0, sizeof(dummy_space));
    (void)nos_object_init(&ev_notif.object, true);

    /* 1. 64KB 连续物理块 + 单页独立 CoW 分裂 + Seal FNV-1a 指纹 */
    c64 = nos_vmo_create(&c64_args);
    assert(c64 != NULL);
    assert(c64->pages[0]->backing_block != NULL);
    assert(c64->pages[0]->backing_block == c64->pages[15]->backing_block);
    assert(c64->pages[0]->backing_block->order == 4U);
    assert(c64->pages[15]->paddr == c64->pages[0]->paddr + 15U * 4096U);

    assert(nos_vmo_write(c64, 0U, "BLOCK64K_BASE", 14U) == 0);
    c64_cow = nos_vmo_clone_cow(c64);
    assert(c64_cow != NULL);
    /* 写第 3 页引发块内单页 CoW 分裂：仅第 3 页脱离 backing_block */
    assert(nos_vmo_write(c64_cow, 3U * 4096U, "COW_PAGE3", 10U) == 0);
    assert(c64_cow->pages[3]->backing_block == NULL);
    assert(c64_cow->pages[4]->backing_block == c64->pages[4]->backing_block);

    assert(nos_vmo_seal(c64) == 0);
    hash1 = nos_vmo_content_hash(c64);
    assert(hash1 != 0ULL && hash1 == nos_vmo_content_hash(c64));

    /* 验证 64KB 连续物理区一次性整段 hal_vm_map */
    assert(nos_vmo_map(c64, &dummy_space, 0x50000000UL, 0U, 16U * 4096U,
                       HAL_VM_PROT_USER | HAL_VM_PROT_READ, NULL) == 0);
    assert(g_map_call_count == 1U && g_maps[0].size == 16U * 4096U);
    assert(nos_vmo_unmap(&dummy_space, 0x50000000UL, 16U * 4096U, NULL) == 0);

    release_vmo(c64_cow);
    release_vmo(c64);

    /* 2. LAZY_ALLOC 按需分配 + ensure_page + AF 位二次机会冷页驱逐 */
    reset_mock_env();
    lazy = nos_vmo_create(&lazy_args);
    assert(lazy != NULL);
    assert(lazy->pages[0] == NULL && lazy->pages[1] == NULL);
    assert(nos_vmo_bind_evict_notif(lazy, &ev_notif) == 0);

    /* map LAZY_ALLOC 不提前分配也不扣完整虚拟大小配额 */
    assert(nos_vmo_map(lazy, &dummy_space, 0x60000000UL, 0U, 4U * 4096U,
                       HAL_VM_PROT_USER | HAL_VM_PROT_READ | HAL_VM_PROT_WRITE,
                       (struct nos_cell *)0x1234UL) == 0);
    assert(g_mock_cell_mem_used == 0ULL);

    /* ensure 第 0、1 页并手工映射（模拟缺页完成），第 0 页标记 AF=1（热），第 1 页保持 AF=0（冷） */
    assert(nos_vmo_ensure_page(lazy, 0U, &ensure_pa, &newly) == 0 && newly);
    assert(hal_vm_map(&dummy_space, 0x60000000UL, ensure_pa, 4096U, HAL_VM_PROT_USER | HAL_VM_PROT_READ) == 0);
    assert(nos_vmo_ensure_page(lazy, 1U, &ensure_pa, &newly) == 0 && newly);
    assert(hal_vm_map(&dummy_space, 0x60001000UL, ensure_pa, 4096U, HAL_VM_PROT_USER | HAL_VM_PROT_READ) == 0);
    assert(nos_vmo_write(lazy, 0U, "HOT_P0", 7U) == 0);
    assert(nos_vmo_write(lazy, 4096U, "COLD_P1", 8U) == 0);
    g_mock_af_bits = (1U << 0);

    /* 第 1 轮扫描：P0 清 AF 并保留，P1 因 AF==0 直接被驱逐；第 2 轮扫描：P0 变冷也被驱逐 */
    assert(nos_vmo_reclaim_cold_pages(lazy, &dummy_space, 0x60000000UL, 8U) == 1);
    assert(lazy->pages[0] != NULL && lazy->pages[1] == NULL);
    assert(nos_vmo_read(lazy, 0U, rb, 7U) == 0 && strcmp((const char *)rb, "HOT_P0") == 0);
    assert(g_mock_evict_signals == 1ULL);

    assert(nos_vmo_reclaim_cold_pages(lazy, &dummy_space, 0x60000000UL, 8U) == 1);
    assert(lazy->pages[0] == NULL && g_mock_evict_signals == 2ULL);
    release_vmo(lazy);

    /* 3. SYNC_DMA 验证 */
    dma = nos_vmo_create(&dma_args);
    assert(dma != NULL);
    assert(nos_vmo_sync_dma(dma, 0U, 2U * 4096U, NOS_DMA_TO_DEVICE) == 0);
    assert(nos_vmo_sync_dma(dma, 0U, 4096U, NOS_DMA_FROM_DEVICE) == 0);
    assert(g_mock_dcache_ops[NOS_DMA_TO_DEVICE] == 2U);
    assert(g_mock_dcache_ops[NOS_DMA_FROM_DEVICE] == 1U);
    release_vmo(dma);
}

int main(void)
{
    test_vmo_seal_and_wx();
    test_vmo_cow_and_kv_prefix();
    test_vmo_quota_and_rollback();
    test_vmo_stage6_blocks_lazy_hash_sync_and_af_reclaim();
    printf("PASS: test_vmo (Seal+Hash, W^X, 64K/CoW split, LazyAlloc, SyncDMA, AF Reclaim)\n");
    return 0;
}
