/**
 * Copyright (C) 2024-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.ndk
 */

#define pr_fmt(fmt) "[NDK]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/task.h>
#include <kernel/sch.h>
#include <kernel/cpu.h>
#include <kernel/sleep.h>
#include <kernel/errno.h>
#include <kernel/sem.h>
#include <kernel/mm.h>
#include <kernel/device.h>
#include <kernel/init.h>
#include <kernel/clk.h>
#include <string.h>

#include "include/ndk.h"
#include "include/protocol.h"
#include "include/cmd.h"

struct ndk_server {
    struct task_struct *task;
    struct ndk_protocol *protocol;
};

static void ndk_server_task_entry(void *parameter)
{
    struct ndk_server *ndk = (struct ndk_server *)parameter;
    struct ndk_ctrl_msg *ctrl_msg;
    int rc;

    ndk->protocol = ndk_protocol_alloc();
    if (ndk->protocol == NULL) {
        pr_err("alloc ndk_protocol error\r\n");
        return;
    }
    pr_info("ndk server start\r\n");

    while (true) {
        rc = ndk_protocol_wait_ctrl_msg(ndk->protocol, &ctrl_msg);
        if (rc < 0) {
            continue;
        }

        switch (ctrl_msg->type) {
        case NDK_CTRL_MSG_FW_UPDATE:
            pr_info("fw update\r\n");
            ndk_protocol_reply_ack(ndk->protocol, 0, 0);
            rc = cmd_fw_update(ndk->protocol, ctrl_msg->send_data_msg_num,
                ctrl_msg->send_data_msg_size, ctrl_msg->data, ctrl_msg->size);
            if (rc < 0) {
                pr_err("fw update failed, rc=%d\r\n", rc);
            } else {
                pr_info("fw update success\r\n");
            }
            break;
        case NDK_CTRL_MSG_GET_VERSION:
            pr_info("get version\r\n");
            ndk_protocol_reply_ack(ndk->protocol, 0, 0);
            break;
        case NDK_CTRL_MSG_HEART_BEAT:
            pr_info("heart beat\r\n");
            ndk_protocol_reply_ack(ndk->protocol, 0, 0);
            break;
        default:
            pr_err("Unknown msg type: %u\r\n", ctrl_msg->type);
            ndk_protocol_reply_ack(ndk->protocol, 0, 0);
            break;
        }
    }
}

static int ndk_server_init(void)
{
    struct ndk_server *ndk;
    int ret;

    ndk = kmalloc(sizeof(struct ndk_server), GFP_KERNEL);
    if (ndk == NULL) {
        pr_err("alloc ndk_server buf error\r\n");
        return -ENOMEM;
    }

    ndk->task = task_create("ndk_server", ndk_server_task_entry, ndk,
        25, 1024, 10, NULL);
    if (ndk->task == NULL) {
        pr_fatal("creat ndk_server task err\r\n");
        BUG_ON(true);
        return -EINVAL;
    }
    task_ready(ndk->task);

    return 0;
}
task_init(ndk_server_init);
