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
#include <kernel/sleep.h>

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

static int write_img_to_flash(struct device *flash_dev, uint8_t img_index,
    uint32_t addr_index, uint8_t *buf, uint32_t size)
{
    addr_t addr;

    switch (img_index) {
        case BOOTLOADER_INDEX:
            addr = BOOTLOADER_INDEX;
            break;
        case KERNEL_A_INDEX:
            addr = KERNEL_A_ADDR;
            break;
        case KERNEL_B_INDEX:
            addr = KERNEL_B_ADDR;
            break;
        default:
            pr_err("Wrong partition number(%d)\r\n", img_index);
            return -EINVAL;
    }
    addr += addr_index;
    //flash_dev->ops.write(flash_dev, addr, (void *)buf, size);

    return 0;
}

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

    uint32_t addr_index = 0;
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

        if (i != 0) {
            msleep(10);
        }

        rc = ndk_protocol_read_data_msg(protocol, msg, read_size);
        if (rc < 0) {
            pr_err("[%d]: read data msg failed\r\n", i);
            err = rc;
            goto done;
        }
        pr_info("[%d]:id=%u, index=%u, size=%u\r\n", i,
                msg->head.id, msg->index, msg->size);
        if (msg->index != i) {
            if (msg->index < i) {
                i--;
                continue;
            }
            pr_err("[%d]: data msg index error, index=%d\r\n",
                i, msg->index);
            err = -EFAULT;
            goto done;
        }
        if (err < 0) {
            goto done;
        }

        rc = write_img_to_flash(flash_dev, info->index,
            addr_index, msg->data, msg->size);
        if (rc < 0) {
            pr_err("[%d]: write img to flash failed, rc=%d\r\n", i, rc);
            err = rc;
            goto done;
        }
        addr_index += msg->size;
    }

done:
    kfree(msg);
    return err;
}
