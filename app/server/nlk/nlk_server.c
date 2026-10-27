/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NLK (NOS Link Kit) Generic Distributed Soft-Bus System Service Task.
 * Board-independent and bearer-independent; polls abstract nlk_bearer
 * instances and delegates hardware capabilities/bearers to board setup hooks.
 */

#define pr_fmt(fmt) "[NLK]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/task.h>
#include <kernel/sched.h>
#include <kernel/cpu.h>
#include <kernel/sleep.h>
#include <kernel/errno.h>
#include <kernel/init.h>
#include <string.h>

#include <nos/nlk_core.h>
#include <nos/nlk_caps.h>

struct nlk_server {
    struct task_struct   *task;
    struct nlk_node      *node;
    struct nlk_log_ctx    log_cap;
    struct nlk_shell_ctx  shell_cap;
};

static struct nlk_server g_nlk_srv;
static nlk_board_setup_fn g_board_setup_fn;

void nlk_server_register_board_setup(nlk_board_setup_fn setup_fn)
{
    g_board_setup_fn = setup_fn;
}

static void nlk_server_task_entry(void *parameter)
{
    struct nlk_server *srv = (struct nlk_server *)parameter;
    uint32_t tick_cnt = 0U;

    srv->node = nlk_default_node();

    /* Allow board/product layer to register its identity, capabilities, and bearers */
    if (g_board_setup_fn != NULL) {
        (void)g_board_setup_fn(srv->node);
    }

    /* Ensure universal OS system capabilities (Kernel Log & Remote nShell) are present */
    if (nlk_node_find_cap_by_type(srv->node, NLK_CAP_LOG) == NULL) {
        (void)nlk_cap_log_init(srv->node, &srv->log_cap);
    }
    if (nlk_node_find_cap_by_type(srv->node, NLK_CAP_SHELL) == NULL) {
        (void)nlk_cap_shell_init(srv->node, &srv->shell_cap);
    }

    pr_info("NLK server started (node=%s, addr=0x%04x, caps=%u, bearers=%u)\r\n",
            srv->node->desc.name, srv->node->addr,
            srv->node->cap_count, srv->node->bearer_count);

    while (true) {
        int rc = nlk_node_poll_bearers(srv->node, 20U);
        if (rc < 0 && rc != -ETIMEDOUT && rc != -EAGAIN) {
            msleep(20);
        }

        (void)nlk_cap_log_flush_pending(&srv->log_cap);

        tick_cnt++;
        if ((tick_cnt % 10U) == 0U && srv->node->desc.attached_children > 0U) {
            struct nlk_capability *kb_cap =
                nlk_node_find_cap_by_type(srv->node, NLK_CAP_KEYBOARD);
            if (kb_cap != NULL && kb_cap->priv != NULL) {
                (void)nlk_cap_keyboard_broadcast_light_beacon(
                    (struct nlk_keyboard_ctx *)kb_cap->priv, tick_cnt);
            }
        }
    }
}

static int nlk_server_init(void)
{
    memset(&g_nlk_srv, 0, sizeof(g_nlk_srv));
    g_nlk_srv.task = task_create("nlk_server", nlk_server_task_entry,
                                 &g_nlk_srv, 6, 2048, 10, NULL);
    if (g_nlk_srv.task == NULL) {
        pr_fatal("create nlk_server task failed\r\n");
        BUG_ON(true);
        return -EINVAL;
    }
    task_ready(g_nlk_srv.task);
    return 0;
}
task_init(nlk_server_init);
