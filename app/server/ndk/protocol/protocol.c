/**
 * Copyright (C) 2024-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[NDK]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/sleep.h>
#include <kernel/errno.h>
#include <kernel/sem.h>
#include <kernel/mm.h>
#include <kernel/device.h>
#include <kernel/init.h>
#include <kernel/clk.h>
#include <string.h>

#include "include/protocol.h"

#define PROTOCOL_DATA_RETRY_MAX 10
#define PROTOCOL_ACK_RETRY_MAX 10

struct ndk_protocol *ndk_protocol_alloc(void)
{
    struct ndk_usb *usb;
    struct ndk_protocol *protocol;
    int rc;

    usb = ndk_usb_alloc();
    if (usb == NULL) {
        return NULL;
    }

    protocol = kzalloc(sizeof(struct ndk_protocol), GFP_KERNEL);
    if (protocol == NULL) {
        ndk_usb_free(usb);
        return NULL;
    }
    protocol->usb = usb;

    return protocol;
}

void ndk_protocol_free(struct ndk_protocol *protocol)
{
    if (protocol == NULL) {
        return;
    }

    ndk_usb_free(protocol->usb);
    kfree(protocol);
}

int ndk_protocol_wait_ctrl_msg(struct ndk_protocol *protocol,
    struct ndk_ctrl_msg **ctrl_msg)
{
    uint8_t *buf;
    struct ndk_ctrl_msg *msg;
    int rc;

    if (ctrl_msg == NULL) {
        pr_err("ctrl_msg is null\r\n");
        return -EINVAL;
    }

    sem_get(&protocol->usb->ctrl_sem);
    buf = kzalloc(protocol->usb->ctrl_msg_size, GFP_KERNEL);
    memcpy(buf, protocol->usb->ctrl_msg_buf,
        protocol->usb->ctrl_msg_size);
    msg = (struct ndk_ctrl_msg *)buf;

    if (msg->head.type != NDK_CTRL_MSG) {
        pr_err("msg type is not NDK_CTRL_MSG, type=%u\r\n", msg->type);
        kfree(buf);
        return -EFAULT;
    }
    protocol->id = msg->head.id;
    *ctrl_msg = msg;

    return 0;
}

int ndk_protocol_read_data_msg(struct ndk_protocol *protocol,
    struct ndk_data_msg *data_msg, uint16_t size)
{
    int rc;
    int retry = PROTOCOL_DATA_RETRY_MAX;

    if (data_msg == NULL) {
        pr_err("data_msg is null\r\n");
        return -EINVAL;
    }

retry:
    rc = ndk_usb_read(protocol->usb, (uint8_t *)data_msg, size);
    if (rc == -ETIMEDOUT) {
        retry--;
        if (retry > 0) {
            goto retry;
        }
    }
    if (rc < 0) {
        pr_err("read data msg failed, rc=%d\r\n", rc);
        return rc;
    }

    if (data_msg->head.type != NDK_DATA_MSG) {
        pr_err("msg type is not NDK_DATA_MSG, type=%u\r\n",
            data_msg->head.type);
        goto retry;
    }

    return rc;
}

int ndk_protocol_send_data_msg(struct ndk_protocol *protocol,
    struct ndk_data_msg *data_msg)
{
    int rc;
    int retry = PROTOCOL_DATA_RETRY_MAX;

    if (data_msg == NULL) {
        pr_err("data_msg is null\r\n");
        return -EINVAL;
    }

retry:
    rc = ndk_usb_write_timeout(protocol->usb, (uint8_t *)data_msg,
        data_msg->size + sizeof(struct ndk_data_msg));
    if (rc == -ETIMEDOUT) {
        retry--;
        if (retry > 0) {
            goto retry;
        }
    }
    if (rc < 0) {
        pr_err("write data msg failed, rc=%d\r\n", rc);
        return rc;
    }

    return rc;
}

int ndk_protocol_reply_ack(struct ndk_protocol *protocol,
    uint16_t send_data_msg_num, uint32_t send_data_msg_size)
{
    struct ndk_ack_msg ack_msg;
    int rc;
    int retry = PROTOCOL_ACK_RETRY_MAX;

    ack_msg.head.type = NDK_ACK_MSG;
    ack_msg.head.id = protocol->id;
    ack_msg.head.size = sizeof(struct ndk_ack_msg) - sizeof(struct ndk_msg_head);
    ack_msg.head.crc = 0;

    ack_msg.send_data_msg_num = send_data_msg_num;
    ack_msg.send_data_msg_size = send_data_msg_size;
    ack_msg.reserve = 0;

retry:
    rc = ndk_usb_write_timeout(protocol->usb, (uint8_t *)&ack_msg,
        sizeof(struct ndk_ack_msg));
    if (rc == -ETIMEDOUT) {
        retry--;
        if (retry > 0) {
            goto retry;
        }
    }
    if (rc < 0) {
        pr_err("write ack msg failed, rc=%d\r\n", rc);
        return rc;
    }

    return rc;
}
