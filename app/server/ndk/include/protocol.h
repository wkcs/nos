/**
 * Copyright (C) 2024-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NDK_PROTOCOL_H__
#define __NDK_PROTOCOL_H__

#include <kernel/sem.h>
#include <kernel/mm.h>

#include "include/winusb.h"

#define  NDK_PACKAGE_SIZE_MAX 1024

enum ndk_msg_type {
    NDK_CTRL_MSG = 0,
    NDK_DATA_MSG,
    NDK_ACK_MSG,
    NDK_MSG_TYPE_MAX,
};

enum ndk_ctrl_msg_type {
    NDK_CTRL_MSG_HEART_BEAT = 0,
    NDK_CTRL_MSG_GET_VERSION,
    NDK_CTRL_MSG_FW_UPDATE,
    NDK_CTRL_MSG_TYPE_MAX,
};

struct ndk_msg_head {
    uint8_t type;
    uint8_t id;
    uint16_t size;
    uint32_t crc;
} __attribute__ ((packed));

struct ndk_ctrl_msg {
    struct ndk_msg_head head;
    uint8_t type;
    uint8_t subtype;
    uint16_t send_data_msg_num;
    uint32_t send_data_msg_size;
    uint16_t reserve;
    uint16_t size;
    uint8_t data[0];
} __attribute__ ((packed));
#define NDK_CTRL_MSG_DATA_SIZE_MAX \
    (NDK_PACKAGE_SIZE_MAX - sizeof(struct ndk_ctrl_msg))

struct ndk_data_msg {
    struct ndk_msg_head head;
    uint16_t index;
    uint16_t size;
    uint8_t data[0];
} __attribute__ ((packed));
#define NDK_DATA_MSG_DATA_SIZE_MAX \
    (NDK_PACKAGE_SIZE_MAX - sizeof(struct ndk_data_msg))

struct ndk_ack_msg {
    struct ndk_msg_head head;
    uint16_t send_data_msg_num;
    uint16_t reserve;
    uint32_t send_data_msg_size;
} __attribute__ ((packed));

struct ndk_protocol {
    struct ndk_usb *usb;
    uint8_t id;
};

struct ndk_protocol *ndk_protocol_alloc(void);
void ndk_protocol_free(struct ndk_protocol *protocol);
int ndk_protocol_wait_ctrl_msg(struct ndk_protocol *protocol,
    struct ndk_ctrl_msg **ctrl_msg);
int ndk_protocol_read_data_msg(struct ndk_protocol *protocol,
    struct ndk_data_msg *data_msg, uint16_t size);
int ndk_protocol_send_data_msg(struct ndk_protocol *protocol,
    struct ndk_data_msg *data_msg);
int ndk_protocol_reply_ack(struct ndk_protocol *protocol,
    uint16_t send_data_msg_num, uint32_t send_data_msg_size);

#endif /* __NDK_PROTOCOL_H__ */
