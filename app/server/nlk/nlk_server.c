/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NLK (NOS Link Kit) Generic Distributed Soft-Bus System Service Task.
 * Board-independent and bearer-independent; polls abstract nlk_bearer
 * instances and delegates hardware capabilities/bearers to board setup hooks.
 */

#define pr_fmt(fmt) "[NLK-SRV] " fmt

#include <kernel/task.h>
#include <kernel/sched.h>
#include <kernel/cpu.h>
#include <kernel/boot.h>
#include <kernel/sleep.h>
#include <kernel/timer.h>
#include <kernel/clk.h>
#include <kernel/errno.h>
#include <kernel/init.h>
#include <kernel/mm.h>
#include <string.h>

#include <nos/nlk_core.h>
#include <nos/nlk_caps.h>
#ifdef CONFIG_SHELL
#include <nos/shell_session.h>
#endif

struct nlk_server {
    struct task_struct *task;
    struct nlk_node *node;
    struct timer reboot_timer;
    uint32_t pending_boot_flag;
    struct nlk_log_ctx log_cap;
    struct nlk_shell_ctx shell_cap;
#ifdef CONFIG_VFS
    struct nlk_vfs_ctx vfs_cap;
#endif
};

static struct nlk_server g_nlk_srv;
static nlk_board_setup_fn g_board_setup_fn;

void nlk_server_register_board_setup(nlk_board_setup_fn setup_fn)
{
    g_board_setup_fn = setup_fn;
}

static void nlk_server_reboot_timer_cb(void *parameter)
{
    struct nlk_server *srv = (struct nlk_server *)parameter;
    uint32_t flag = (srv != NULL) ? srv->pending_boot_flag : BOOT_NORMAL;

    cpu_reboot(flag);
}

static void nlk_server_default_reboot(uint32_t boot_flag, void *priv)
{
    struct nlk_server *srv = (struct nlk_server *)priv;

    if (srv == NULL) {
        return;
    }
    srv->pending_boot_flag = boot_flag;
    timer_start(&srv->reboot_timer, msec_to_tick(50));
}

#ifdef CONFIG_SHELL
static int nlk_server_start_shell(void *priv)
{
    struct nlk_shell_ctx *ctx = priv;
    return nshell_spawn(nlk_cap_shell_tty(ctx), "nshell_nlk", false);
}
#endif

static void nlk_server_task_entry(void *parameter)
{
    struct nlk_server *srv = (struct nlk_server *)parameter;
    uint64_t next_health_ms = 0U;

    srv->node = nlk_default_node();
    srv->pending_boot_flag = BOOT_NORMAL;
    timer_init(&srv->reboot_timer, "nlk_rb", nlk_server_reboot_timer_cb, srv);
    nlk_node_set_reboot_handler(srv->node, nlk_server_default_reboot, srv);

    /* 允许板级/产品层注册节点身份、分区与外设能力及物理 Bearer */
    if (g_board_setup_fn != NULL) {
        (void)g_board_setup_fn(srv->node);
    }

    /* 注册系统通用能力（Kernel Log、nShell 前端及 VFS 远端文件服务） */
    if (nlk_node_find_cap_by_type(srv->node, NLK_CAP_LOG) == NULL) {
        (void)nlk_cap_log_init(srv->node, &srv->log_cap);
    }
    if (nlk_node_find_cap_by_type(srv->node, NLK_CAP_SHELL) == NULL) {
        (void)nlk_cap_shell_init(srv->node, &srv->shell_cap);
    }
#ifdef CONFIG_SHELL
    struct nlk_shell_ctx *shell = nlk_cap_shell_default_ctx();
    nlk_cap_shell_set_frontend_start(shell, nlk_server_start_shell, shell);
#endif
#ifdef CONFIG_VFS
    if (nlk_node_find_cap_by_type(srv->node, NLK_CAP_VFS) == NULL) {
        (void)nlk_cap_vfs_init(srv->node, &srv->vfs_cap);
    }
#endif

    pr_info("NLK server started (node=%s, addr=0x%04x, caps=%u, bearers=%u)\n",
            srv->node->desc.name, srv->node->addr, srv->node->cap_count, srv->node->bearer_count);

    while (true) {
        uint64_t health_ms = tick_to_msec_64(cpu_run_ticks());

        if (health_ms >= next_health_ms) {
            next_health_ms = health_ms + 10000U;
            pr_info("alive rx=%u tx=%u drop=%u heap_free=%u\n", srv->node->rx_packets,
                    srv->node->tx_packets, srv->node->dropped_packets,
                    (unsigned int)mm_block_free_size());
        }
        int rc = nlk_node_poll_bearers(srv->node, 20U);
        if (rc <= 0) {
            msleep(20);
        }

        {
            struct nlk_capability *shell_cap = nlk_node_find_cap_by_type(srv->node, NLK_CAP_SHELL);
            struct nlk_shell_ctx *ctx = nlk_cap_shell_default_ctx();
            if (shell_cap != NULL && ctx != NULL && shell_cap->priv == ctx) {
                nlk_cap_shell_tick(ctx, (uint32_t)tick_to_msec_64(cpu_run_ticks()));
                (void)nlk_cap_shell_flush_pending(ctx);
            }
        }
        (void)nlk_cap_log_flush_pending(&srv->log_cap);

        nlk_node_poll_capabilities(srv->node, (uint32_t)tick_to_msec_64(cpu_run_ticks()));
    }
}

static int nlk_server_init(void)
{
    memset(&g_nlk_srv, 0, sizeof(g_nlk_srv));
    /* The board sizes this stack for all registered capability call chains. */
    g_nlk_srv.task = task_create("nlk_server", nlk_server_task_entry, &g_nlk_srv, 6,
                                 CONFIG_NLK_SERVER_STACK_SIZE, 10, NULL);
    if (g_nlk_srv.task == NULL) {
        pr_fatal("create nlk_server task failed\n");
        BUG_ON(true);
        return -EINVAL;
    }
    task_ready(g_nlk_srv.task);
    return 0;
}
task_init(nlk_server_init);
