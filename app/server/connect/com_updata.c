/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/errno.h>
#include <kernel/mm.h>
#include <kernel/sch.h>
#include <kernel/sleep.h>
#include <kernel/device.h>
#include <kernel/cpu.h>
#include "com.h"

#define IMG_MAGIC 0x20302030

#define BOOTLOADER_INDEX 0
#define KERNEL_A_INDEX   1
#define KERNEL_B_INDEX   2

#define BOOTLOADER_ADDR 0x0
#define KERNEL_A_ADDR   0x8000
#define KERNEL_B_ADDR   0x3A000

#define REBOOT_FLASH_BASE 0x7f00000

struct img_head_info {
    uint32_t magic;
    uint32_t check_sum;
    uint32_t size;
    uint8_t type;
    uint8_t version[3];
} __attribute__ ((packed));

static int write_img_to_flash(struct com_data *com, uint8_t *img_buf, uint32_t size, uint32_t addr_index, uint8_t img_index)
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
    if (com->flash_dev->ops.write == NULL) {
        pr_err("%s not susport write\r\n", com->flash_dev->name);
        return -EIO;
    }
    com->flash_dev->ops.write(com->flash_dev, addr, (void *)img_buf, size);

    return 0;
}

void write_reboot_flag(struct com_data *com, uint8_t reboot_flag)
{
    com->flash_dev->ops.write(com->flash_dev, REBOOT_FLASH_BASE, (void *)&reboot_flag, 1);
}

int com_download_img(struct com_data *com, uint8_t *buf, uint32_t size, uint32_t index)
{
    static uint32_t index_save;
    static uint32_t total_size;
    static addr_t addr;
    static uint8_t img_index;
    int rc;

    if (index == 0) {
        if (index_save != 0) {
            pr_err("pkg error, exit flash\r\n");
            index_save = 0;
            //com_send_ack(true);
            return -EINVAL;
        }
        index_save++;
        total_size = (uint32_t)(buf[3] << 24) | (uint32_t)(buf[2] << 16) | (uint32_t)(buf[1] << 8) | buf[0];
        img_index = buf[4];
        addr = 0;
        pr_info("img size:%d\r\n", total_size);
        //com_send_ack(false);
        return 0;
    }

    pr_info("img index:%d, size:%d\r\n", index, size);
    rc = write_img_to_flash(com, buf, size, addr, img_index);
    if (rc < 0) {
        pr_err("flash img error, rc=%d\r\n", rc);
        //com_send_ack(true);
        index_save = 0;
        return rc;
    }
    /* if (index_save < index) {
        pr_err("pkg error, exit flash, index=%d, need=%d\r\n",index, index_save);
        index_save = 0;
        //com_send_ack(true);
        return -EINVAL;
    } */
    index_save++;
    //com_send_ack(false);
    addr += size;
    if (addr == total_size) {
        pr_info("download img success, reboot now\r\n");
        // write_reboot_flag(com, img_index);
        cpu_reboot(REBOOT_TO_BOOTLOADER);
    }

    return 0;
}
