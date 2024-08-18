/**
 * Copyright (C) 2024-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.ndk
 */

#define pr_fmt(fmt) "[NDK]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/types.h>
#include <kernel/device.h>

#include "include/ndk.h"
#include "include/protocol.h"

#define IMG_MAGIC 0x20302030

#define BOOTLOADER_INDEX 0
#define KERNEL_A_INDEX   1
#define KERNEL_B_INDEX   2

#define BOOTLOADER_ADDR 0x0
#define KERNEL_A_ADDR   0x8000
#define KERNEL_B_ADDR   0x3A000

#define REBOOT_FLASH_BASE 0x7f00000

struct fw_update_info {
    uint32_t index;
    uint32_t size;
} __attribute__ ((packed));

int cmd_fw_update(struct ndk_protocol *protocol,
    uint16_t send_data_msg_num, uint32_t send_data_msg_size,
    const char *buf, uint32_t size)
{
    const struct fw_update_info *info = (const struct fw_update_info *)buf;
    struct device *flash_dev;
    struct ndk_data_msg *msg;
    int err = 0;
    int i;
    int rc;

    if (size != sizeof(struct fw_update_info)) {
        pr_err("data size error\r\n");
        return -EINVAL;
    }

    pr_info("send_data_msg_num=%u, send_data_msg_size=%u\r\n",
            send_data_msg_num, send_data_msg_size);
    pr_info("[fw_update_info]: index=%u, size=%u\r\n",
            info->index, info->size);

    flash_dev = device_find_by_name(CONFIG_FLASH_DEV);
    if (flash_dev == NULL) {
        pr_err("%s not found\r\n", CONFIG_FLASH_DEV);
        return -ENODEV;
    }

    msg = kmalloc(NDK_PACKAGE_SIZE_MAX, GFP_KERNEL);
    if (msg == NULL) {
        pr_err("alloc msg buf failed\r\n");
        return -ENOMEM;
    }

    for (i = 0; i < send_data_msg_num; i++) {
        uint32_t read_size;

        if (send_data_msg_size <= NDK_PACKAGE_SIZE_MAX) {
            read_size = send_data_msg_size;
        } else {
            read_size = send_data_msg_size - NDK_PACKAGE_SIZE_MAX * i;
            if (read_size > NDK_PACKAGE_SIZE_MAX) {
                read_size = NDK_PACKAGE_SIZE_MAX;
            }
        }

        rc = ndk_protocol_read_data_msg(protocol, msg, send_data_msg_size);
        if (rc < 0) {
            pr_err("[%d]: read data msg failed\r\n", i);
            err = rc;
            continue;
        }
        if (msg->index != i) {
            pr_err("[%d]: data msg index error, index=%d\r\n",
                i, msg->index);
            err = -EFAULT;
            continue;
        }
        pr_info("[%d]:id=%u, index=%u, size=%u\r\n", i,
                msg->head.id, msg->index, msg->size);
    }

    kfree(msg);

    return err;
}
