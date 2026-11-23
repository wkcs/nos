/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * 独立编译的 ET_DYN PIE AArch64 EL0 Tool Server ELF (/bin/tool_srv.elf)
 * 验证：ET_DYN PIE (R_AARCH64_RELATIVE 重定位)、argv 传递、ulibc LAZY_ALLOC 堆、
 *       以及跨 Cell EL0 RingIPC BIND_SERVER / SERVER_POP / COMPLETE 服务端闭环。
 */

#include <nos/ulibc.h>

#define TOOL_SRV_LOCAL_RING_VA 0x0000000000580000UL

static int srv_add_handler(uint64_t a, uint64_t b, uint32_t *out_len)
{
    if (out_len == NULL) {
        return -EINVAL;
    }
    *out_len = (uint32_t)(a + b);
    return 0;
}

struct tool_srv_route {
    const char *tag;
    int (*fn)(uint64_t a, uint64_t b, uint32_t *out_len);
};

/* 指针常量表：配合 volatile 索引阻止 -O2 折叠，强制链接器产出真实 R_AARCH64_RELATIVE 条目 */
static const char *const g_srv_tags[2] = { "nos-tool-srv", "pie-ok" };
static struct tool_srv_route g_routes[2] = {
    { "sum", srv_add_handler },
    { "alt", srv_add_handler },
};
static volatile uint32_t g_pie_idx;

int nos_user_main(const struct nos_user_boot_info *boot)
{
    const char *const *argv;
    uint32_t idx;
    char *scratch;
    int rc;

    if (boot == NULL || boot->magic != NOS_USER_BOOT_MAGIC ||
        boot->argc < 1U || boot->argv_va == 0ULL) {
        return 10;
    }

    /* 1. 核验 ET_DYN PIE 运行期数据与函数指针重定位均已准确生效 */
    idx = g_pie_idx;
    if (idx > 1U) {
        return 11;
    }
    if (ustrcmp(g_srv_tags[idx], "nos-tool-srv") != 0 ||
        ustrcmp(g_srv_tags[idx + 1U], "pie-ok") != 0 ||
        ustrcmp(g_routes[idx].tag, "sum") != 0 ||
        g_routes[idx].fn == NULL) {
        return 11;
    }

    argv = (const char *const *)(uintptr_t)boot->argv_va;
    if (argv[0] == NULL || ustrlen(argv[0]) == 0U || argv[boot->argc] != NULL) {
        return 12;
    }

    scratch = (char *)umalloc(128U);
    if (scratch == NULL) {
        return 13;
    }
    umemset(scratch, 0x5A, 64U);
    if ((uint8_t)scratch[0] != 0x5AU || (uint8_t)scratch[63] != 0x5AU) {
        return 14;
    }
    ufree(scratch);

    /* 2. 若挂载共享 RingIPC 句柄则进入 EL0 服务端模式 */
    if (boot->ring_cap != NOS_HANDLE_INVALID) {
        struct nos_ring_sqe sqe;
        struct nos_ring_cqe cqe;
        uint32_t res_len = 0U;

        rc = nos_user_ring_bind_server(boot->ring_cap);
        if (rc != 0) {
            return 15;
        }

        umemset(&sqe, 0, sizeof(sqe));
        rc = nos_user_ring_server_pop(boot->ring_cap, &sqe, 1U, 5000U);
        if (rc != 1) {
            return 16;
        }

        umemset(&cqe, 0, sizeof(cqe));
        cqe.req_id = sqe.req_id;
        cqe.status = g_routes[idx].fn(sqe.inline_arg[0], sqe.inline_arg[1], &res_len);
        cqe.result_len = res_len;

        rc = nos_user_ring_complete(boot->ring_cap, &cqe);
        if (rc != 0) {
            return 17;
        }

        uputs("[EL0-TOOL-SRV] PIE+argv+RingIPC server completed req OK!\n");
        return 0;
    }

    /* 3. 自检模式（nShell `runelf /bin/tool_srv.elf --self-check`） */
    if (boot->argc >= 2U && argv[1] != NULL && ustrcmp(argv[1], "--self-check") != 0) {
        return 18;
    }

    {
        struct nos_ring_create_args rargs;
        nos_handle_t local_ring = NOS_HANDLE_INVALID;
        uint32_t res_len = 0U;

        umemset(&rargs, 0, sizeof(rargs));
        rargs.sq_entries = 16U;
        rargs.cq_entries = 16U;
        rargs.map_user_vaddr = TOOL_SRV_LOCAL_RING_VA;

        if (nos_user_ring_create(&rargs, &local_ring) != 0 ||
            local_ring == NOS_HANDLE_INVALID) {
            return 19;
        }
        if (nos_user_ring_bind_server(local_ring) != 0) {
            return 20;
        }
        if (g_routes[idx].fn(19ULL, 23ULL, &res_len) != 0 || res_len != 42U) {
            return 21;
        }
    }

    uputs("[EL0-TOOL-SRV] PIE+argv self-check OK!\n");
    return 0;
}
