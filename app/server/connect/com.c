/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[CONNECT]:%s[%d]:"fmt, __func__, __LINE__

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

#include "com.h"

struct com_data *g_com_data;

static void com_read_complete(struct device *dev, size_t size)
{
    sem_send(&g_com_data->read_sem, 1);
}

static void com_write_complete(struct device *dev, const void *buffer)
{
    sem_send(&g_com_data->write_sem, 1);
}

void com_read_data_all_wait(uint8_t *buf, size_t size)
{
    g_com_data->usb_dev->ops.read(g_com_data->usb_dev, 0, buf, size);
    sem_get(&g_com_data->read_sem);
}

int com_read_data(uint8_t *buf, size_t size)
{
    int ret;

    g_com_data->usb_dev->ops.read(g_com_data->usb_dev, 0, buf, size);
    ret = sem_get_timeout(&g_com_data->read_sem, msec_to_tick(100));
    if (ret != 0) {
        // pr_err("winusb read data timedout\r\n");
        return -ETIMEDOUT;
    }

    return 0;
}

int com_write_data(const uint8_t *buf, size_t size)
{
    int rc;

    g_com_data->usb_dev->ops.write(g_com_data->usb_dev, 0, buf, size);
    rc = sem_get_timeout(&g_com_data->write_sem, sec_to_tick(2));
    if (rc != 0) {
        pr_err("winusb write data timedout, rc=%d\r\n", rc);
        return -ETIMEDOUT;
    }

    return 0;
}

int com_send_ack(bool err)
{
    int ret;
    struct com_cmd_package cmd_ack_pkg;

    cmd_ack_pkg.type = COM_ACK_PKG;
    if (err)
        cmd_ack_pkg.data_type = DATA_ERR;
    else
        cmd_ack_pkg.data_type = DATA0;
    cmd_ack_pkg.index = 0;
    cmd_ack_pkg.size = 0;
    ret = com_write_data((uint8_t *)&cmd_ack_pkg, sizeof(cmd_ack_pkg));
    if (ret != 0)
        pr_err("send ack timedout\r\n");

    return ret;
}

int com_wait_ack(void)
{
    int ret;
    struct com_cmd_package cmd_ack_pkg;

    ret = com_read_data((uint8_t *)&cmd_ack_pkg, sizeof(cmd_ack_pkg));
    if (ret != 0) {
        pr_err("wait ack timedout\r\n");
        return ret;
    }

    if (cmd_ack_pkg.type != COM_ACK_PKG) {
        pr_err("ack package err\r\n");
        return -EINVAL;
    }

    return 0;
}

static void connect_server_task_entry(void *parameter)
{
    int ret;
    struct com_cmd_package cmd_pkg;
    uint8_t *data_buf = NULL;
    static uint8_t data_type;
    struct com_data *com = parameter;
    int i;
    bool download_start = false;

    com->flash_dev = NULL;
    for (i = 0; i < 100; i ++) {
        com->flash_dev = device_find_by_name(CONFIG_FLASH_DEV);
        if (com->flash_dev == NULL) {
            i++;
            pr_err("%s device not found, retry=%d\r\n", CONFIG_FLASH_DEV, i);
            sleep(1);
            continue;
        } else {
            pr_info("%s device found\r\n", CONFIG_FLASH_DEV);
            break;
        }
    }
    if (i >= 100) {
        pr_err("%s device not found, exit\r\n", CONFIG_FLASH_DEV);
        return;
    }

    com->usb_dev = NULL;
    for (i = 0; i < 100; i ++) {
        com->usb_dev = device_find_by_name("winusb");
        if (com->usb_dev == NULL) {
            i++;
            pr_err("%s device not found, retry=%d\r\n", "winusb", i);
            sleep(1);
            continue;
        } else {
            pr_info("%s device found\r\n", "winusb");
            break;
        }
    }
    if (i >= 100) {
        pr_err("%s device not found, exit\r\n", CONFIG_FLASH_DEV);
        return;
    }
    com->usb_dev->ops.read_complete = com_read_complete;
    com->usb_dev->ops.write_complete = com_write_complete;

    data_buf = (uint8_t *)alloc_page(GFP_KERNEL);
    pr_info("data_buf addr is 0x%08lx - 0x%08lx\r\n", (addr_t)data_buf, (addr_t)data_buf + CONFIG_PAGE_SIZE);
    while (1) {
        ret = com_read_data(data_buf, sizeof(struct com_cmd_package));
        if (ret != 0) {
            continue;
        }
        memcpy(&cmd_pkg, data_buf, sizeof(struct com_cmd_package));
        pr_info("get com package ok, type=%d, index=%d, size=%d\r\n", cmd_pkg.type, cmd_pkg.index, cmd_pkg.size);
        if (cmd_pkg.size != 0) {
            if (cmd_pkg.size > CONFIG_PAGE_SIZE) {
                pr_err("data size to max\r\n");
                continue;
            }
            ret = com_read_data(data_buf, cmd_pkg.size);
            if (ret != 0) {
                pr_err("can't get pkg data, rc=%d\r\n", ret);
                continue;
            }
        }
        switch(cmd_pkg.type) {
            case COM_HEART_PKG:
                com_send_ack(false);
                data_type = DATA0;
                break;
            case COM_SET_CONFIG_PKG:
                pr_info("set config\r\n");
                data_type = DATA0;
                break;
            case COM_GET_CONFIG_PKG:
                pr_info("get config\r\n");
                data_type = DATA0;
                break;
            case COM_GET_VERSION_PKG:
                com_get_version();
                data_type = DATA0;
                break;
            case COM_DOWNLOAD_PKG:
                if (!download_start) {
                    download_start = true;
                    com->info.img_index = 0;
                    com->info.index_save = 0;
                    com->info.total_size = 0;
                    com->info.addr = 0;
                    data_type = DATA0;
                }
                switch (cmd_pkg.data_type) {
                case DATA0:
                    if (data_type != DATA0) {
                        pr_err("data type is error, type=%d, need=%d\r\n", cmd_pkg.data_type, data_type);
                        download_start = false;
                    } else {
                        data_type = DATA1;
                    }
                    break;
                case DATA1:
                    if (data_type != DATA1) {
                        pr_err("data type is error, type=%d, need=%d\r\n", cmd_pkg.data_type, data_type);
                        download_start = false;
                    } else {
                        data_type = DATA0;
                    }
                    break;
                default:
                    pr_err("data type is error, type=%d, need=%d\r\n", cmd_pkg.data_type, data_type);
                    download_start = false;
                    continue;
                }
                ret = com_download_img(com, data_buf, cmd_pkg.size, cmd_pkg.index);
                if (ret < 0) {
                    pr_err("download error, rc=%d\r\n", ret);
                    download_start = false;
                }
                break;
            case COM_DATA_PKG:
                break;
            default:
                pr_err("unknown package(%x)\r\n", cmd_pkg.type);
                break;
        }
    }
}

static int connect_server_init(void)
{
    struct com_data *com;
    int ret;

    com = kmalloc(sizeof(struct com_data), GFP_KERNEL);
    if (com == NULL) {
        pr_err("alloc com buf error\r\n");
        return -ENOMEM;
    }
    g_com_data = com;

    sem_init(&com->read_sem, 0);
    sem_init(&com->write_sem, 0);

    com->task = task_create("connect_server", connect_server_task_entry, com, 15, 4096, 10, NULL);
    if (com->task == NULL) {
        pr_fatal("creat connect_server task err\r\n");
        BUG_ON(true);
        return -EINVAL;
    }
    task_ready(com->task);

    return 0;
}
task_init(connect_server_init);
