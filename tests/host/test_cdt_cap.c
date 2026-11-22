/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * 宿主端 TDD 测试：双层可扩 CNode (最多 4096 槽位) 与 CDT 可撤销能力派生树
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uapi/nos/errno.h>
#include <uapi/nos/abi.h>
#include <uapi/nos/ai.h>
#include <kernel/mm.h>
#include <nos/object.h>
#include <nos/cap.h>

_Static_assert(sizeof(void *) != 8 || sizeof(struct nos_cap_slot) == 64,
               "64 位架构下 nos_cap_slot 必须严格对齐至单条 64 字节 Cache Line");

/* 测试桩实现：提供内核分配器与日志符号 */
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

struct mock_audit_log {
    uint32_t derive_calls;
    uint32_t revoke_calls;
    uint64_t last_revoked_count;
    void    *last_cell;
    uint32_t last_badge;
};

static struct mock_audit_log g_audit_log;

static void test_audit_hook(void *cell,
                            uint16_t event_type,
                            uint64_t obj_id,
                            uint32_t badge,
                            int32_t status,
                            uint64_t arg0,
                            uint64_t arg1)
{
    (void)obj_id;
    (void)status;
    (void)arg0;
    g_audit_log.last_cell = cell;
    g_audit_log.last_badge = badge;
    if (event_type == NOS_AUDIT_CAP_DERIVE) {
        g_audit_log.derive_calls++;
    } else if (event_type == NOS_AUDIT_CAP_REVOKE) {
        g_audit_log.revoke_calls++;
        g_audit_log.last_revoked_count = arg1;
    }
}

static void test_expandable_4096_slots(void)
{
    struct nos_cap_table tbl;
    struct nos_object dummy_vmo;
    static nos_handle_t handles[NOS_CAP_CDT_MAX_SLOTS];
    nos_handle_t extra;
    int allocated = 0;
    int rc;
    int i;

    nos_object_init(&dummy_vmo, false);
    nos_cap_table_init(&tbl);

    /* 槽位 0 为无效句柄索引，总可用槽位应为 1 .. 4095 共 4095 个 */
    for (i = 0; i < (int)NOS_CAP_CDT_MAX_SLOTS; i++) {
        rc = nos_cap_alloc(&tbl, NOS_CAP_VMO, &dummy_vmo, NOS_CAP_RIGHT_READ, 0U, &extra);
        if (rc == 0) {
            handles[allocated++] = extra;
        } else {
            assert(rc == -ENOSPC);
            break;
        }
    }

    assert(allocated == (int)(NOS_CAP_CDT_MAX_SLOTS - 1U));
    assert(tbl.cap_capacity == NOS_CAP_CDT_MAX_SLOTS);
    assert(nos_cap_alloc(&tbl, NOS_CAP_VMO, &dummy_vmo, NOS_CAP_RIGHT_READ, 0U, &extra) == -ENOSPC);

    /* 校验第 1 个与最后一个扩展页句柄均能正常查表并 Pinned 递增引用 */
    for (i = 0; i < allocated; i += 500) {
        void *out = NULL;
        assert(nos_cap_lookup(&tbl, handles[i], NOS_CAP_VMO, NOS_CAP_RIGHT_READ, &out, NULL) == 0);
        assert(out == &dummy_vmo);
        nos_object_put(&dummy_vmo);
    }

    nos_cap_table_destroy(&tbl);
}

static void test_cdt_cross_table_revoke_and_reparent(void)
{
    struct nos_cap_table root_tbl;
    struct nos_cap_table cell_a_tbl;
    struct nos_cap_table cell_b_tbl;
    struct nos_object obj_main;
    nos_handle_t h_root;
    nos_handle_t h_a_branch;
    nos_handle_t h_b_leaf1;
    nos_handle_t h_b_leaf2;
    nos_handle_t h_sibling;
    uint32_t revoked = 0U;
    void *fake_cell_tag = (void *)0xCAFE0001UL;
    void *out_obj = NULL;
    uint32_t badge = 0U;
    uint32_t out_type = 0U;
    nos_cap_rights_t out_rights = 0U;
    int rc;

    memset(&g_audit_log, 0, sizeof(g_audit_log));
    nos_cap_register_audit_hook(test_audit_hook);

    nos_object_init(&obj_main, false);
    nos_cap_table_init(&root_tbl);
    nos_cap_table_init(&cell_a_tbl);
    nos_cap_table_init(&cell_b_tbl);
    nos_cap_table_bind_cell(&root_tbl, fake_cell_tag);

    assert(nos_cap_alloc(&root_tbl,
                         NOS_CAP_CELL,
                         &obj_main,
                         NOS_CAP_RIGHT_ALL,
                         0U,
                         &h_root) == 0);

    /* 越权放大或不可改 Badge / 无 GRANT 跨表均必须被拒绝 */
    assert(nos_cap_derive(&root_tbl, h_root, &cell_a_tbl, NOS_CAP_RIGHT_READ, 0U, &h_a_branch) == 0);
    assert(nos_cap_derive(&cell_a_tbl, h_a_branch, &cell_b_tbl, NOS_CAP_RIGHT_READ, 0U, &h_b_leaf1) == -EACCES);

    /* 重新从 root_tbl 派生带有 GRANT+READ 权限和固定 Badge=0x42 的子分支 -> 跨表至 A -> 跨表至 B */
    assert(nos_cap_revoke_tree(&cell_a_tbl, h_a_branch, &revoked) == 0);
    assert(revoked == 1U);

    assert(nos_cap_derive(&root_tbl,
                          h_root,
                          &cell_a_tbl,
                          NOS_CAP_RIGHT_READ | NOS_CAP_RIGHT_GRANT,
                          0x42U,
                          &h_a_branch) == 0);
    /* 尝试放大到 WRITE 必须失败 */
    assert(nos_cap_derive(&cell_a_tbl,
                          h_a_branch,
                          &cell_b_tbl,
                          NOS_CAP_RIGHT_READ | NOS_CAP_RIGHT_WRITE,
                          0U,
                          &h_b_leaf1) == -EACCES);
    /* 尝试篡改已有 Badge=0x42 为 0x99 必须失败 */
    assert(nos_cap_derive(&cell_a_tbl,
                          h_a_branch,
                          &cell_b_tbl,
                          NOS_CAP_RIGHT_READ,
                          0x99U,
                          &h_b_leaf1) == -EACCES);

    /* 合法派生到 B 两个叶子并继承 Badge=0x42，同时在 root 另建无关兄弟分支 */
    assert(nos_cap_derive(&cell_a_tbl, h_a_branch, &cell_b_tbl, NOS_CAP_RIGHT_READ, 0U, &h_b_leaf1) == 0);
    assert(nos_cap_derive(&cell_b_tbl, h_b_leaf1, &root_tbl, NOS_CAP_RIGHT_READ, 0U, &h_b_leaf2) == -EACCES);
    assert(nos_cap_derive(&cell_a_tbl, h_a_branch, &cell_b_tbl, NOS_CAP_RIGHT_READ, 0U, &h_b_leaf2) == 0);
    assert(nos_cap_derive(&root_tbl, h_root, &cell_b_tbl, NOS_CAP_RIGHT_READ, 0x77U, &h_sibling) == 0);

    /* 校验 Pinned Lookup 继承的 Badge 和对象引用 */
    assert(nos_cap_lookup(&cell_b_tbl, h_b_leaf1, NOS_CAP_CELL, NOS_CAP_RIGHT_READ, &out_obj, &badge) == 0);
    assert(out_obj == &obj_main && badge == 0x42U);
    nos_object_put(&obj_main);

    /* 仅吊销 A 的子分支树：应原子级联清除 h_a_branch + h_b_leaf1 + h_b_leaf2 共 3 个节点 */
    rc = nos_cap_revoke_tree(&cell_a_tbl, h_a_branch, &revoked);
    assert(rc == 0 && revoked == 3U);
    assert(nos_cap_lookup(&cell_a_tbl, h_a_branch, NOS_CAP_CELL, 0U, NULL, NULL) == -EBADF);
    assert(nos_cap_lookup(&cell_b_tbl, h_b_leaf1, NOS_CAP_CELL, 0U, NULL, NULL) == -EBADF);
    assert(nos_cap_lookup(&cell_b_tbl, h_b_leaf2, NOS_CAP_CELL, 0U, NULL, NULL) == -EBADF);

    /* 根节点 h_root 与另一条兄弟分支 h_sibling 必须完好无损 */
    assert(nos_cap_inspect(&root_tbl, h_root, &out_type, &out_rights) == 0);
    assert(out_type == NOS_CAP_CELL && out_rights == NOS_CAP_RIGHT_ALL);
    assert(nos_cap_lookup(&cell_b_tbl, h_sibling, NOS_CAP_CELL, NOS_CAP_RIGHT_READ, NULL, &badge) == 0);
    assert(badge == 0x77U);

    /* 再建立 A -> B 链后销毁中间表 A（触发 reparent 给 root），再吊销 root 必须能联动清理 B 残留叶子 */
    assert(nos_cap_derive(&root_tbl, h_root, &cell_a_tbl, NOS_CAP_RIGHT_READ | NOS_CAP_RIGHT_GRANT, 0U, &h_a_branch) == 0);
    assert(nos_cap_derive(&cell_a_tbl, h_a_branch, &cell_b_tbl, NOS_CAP_RIGHT_READ, 0U, &h_b_leaf1) == 0);
    nos_cap_table_destroy(&cell_a_tbl);

    /* 此时 h_b_leaf1 已经安全重挂到 h_root 下，revoke_tree(root) 应连带清理 h_root + h_sibling + h_b_leaf1 = 3 */
    rc = nos_cap_revoke_tree(&root_tbl, h_root, &revoked);
    assert(rc == 0 && revoked == 3U);
    assert(g_audit_log.last_cell == fake_cell_tag);
    assert(g_audit_log.last_revoked_count == 3U);
    assert(g_audit_log.revoke_calls == 1U);
    assert(g_audit_log.derive_calls >= 3U);
    assert(nos_cap_lookup(&cell_b_tbl, h_sibling, NOS_CAP_CELL, 0U, NULL, NULL) == -EBADF);
    assert(nos_cap_lookup(&cell_b_tbl, h_b_leaf1, NOS_CAP_CELL, 0U, NULL, NULL) == -EBADF);

    /* 验证 Retired 对象返回 -ESTALE */
    assert(nos_cap_alloc(&root_tbl, NOS_CAP_VMO, &obj_main, NOS_CAP_RIGHT_READ, 0U, &h_root) == 0);
    assert(nos_object_close(&obj_main) == 0);
    assert(nos_object_retire(&obj_main) == 0);
    assert(nos_cap_lookup(&root_tbl, h_root, NOS_CAP_VMO, NOS_CAP_RIGHT_READ, &out_obj, NULL) == -ESTALE);

    nos_cap_register_audit_hook(NULL);
    nos_cap_table_destroy(&root_tbl);
    nos_cap_table_destroy(&cell_b_tbl);
}

static void test_deep_chain_iterative_revoke(void)
{
    struct nos_cap_table tbl_even;
    struct nos_cap_table tbl_odd;
    struct nos_object obj;
    nos_handle_t prev;
    nos_handle_t cur;
    uint32_t revoked = 0U;
    int i;
    const int depth = 600;

    nos_object_init(&obj, false);
    nos_cap_table_init(&tbl_even);
    nos_cap_table_init(&tbl_odd);

    assert(nos_cap_alloc(&tbl_even, NOS_CAP_VMO, &obj, NOS_CAP_RIGHT_ALL, 0U, &prev) == 0);
    nos_handle_t root = prev;

    for (i = 1; i < depth; i++) {
        struct nos_cap_table *src = (i % 2 == 1) ? &tbl_even : &tbl_odd;
        struct nos_cap_table *dst = (i % 2 == 1) ? &tbl_odd : &tbl_even;
        assert(nos_cap_derive(src, prev, dst, NOS_CAP_RIGHT_ALL, 0U, &cur) == 0);
        prev = cur;
    }

    assert(nos_cap_revoke_tree(&tbl_even, root, &revoked) == 0);
    assert(revoked == (uint32_t)depth);
    assert(nos_cap_lookup(&tbl_odd, prev, NOS_CAP_VMO, 0U, NULL, NULL) == -EBADF);

    nos_cap_table_destroy(&tbl_even);
    nos_cap_table_destroy(&tbl_odd);
}

int main(void)
{
    test_expandable_4096_slots();
    test_cdt_cross_table_revoke_and_reparent();
    test_deep_chain_iterative_revoke();
    printf("PASS: test_cdt_cap (4096-slot CNode, cross-table CDT revoke, reparent, iterative DFS)\n");
    return 0;
}
