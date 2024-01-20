/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __COM_COM_H__
#define __COM_COM_H__

#include <kernel/kernel.h>
#include <kernel/sem.h>
#include <kernel/task.h>
#include <kernel/device.h>

struct com_download_info {
    uint32_t index_save;
    uint32_t total_size;
    addr_t addr;
    uint8_t img_index;
};

struct com_data {
    struct task_struct *task;
    struct device *flash_dev;
    struct device *usb_dev;
    sem_t read_sem;
    sem_t write_sem;
    struct com_download_info info;
};

enum com_cmd_type {
    COM_ACK_PKG = 0,
    COM_HEART_PKG,
    COM_SET_CONFIG_PKG,
    COM_GET_CONFIG_PKG,
    COM_GET_VERSION_PKG,
    COM_DOWNLOAD_PKG,
    COM_DATA_PKG,
};

struct com_cmd_package {
    uint8_t type;
#define DATA0    0
#define DATA1    1
#define DATA2    2
#define DATA_ERR 0xff
    uint8_t data_type;
    uint32_t index;
    uint32_t size;
    uint8_t buf[0];
} __attribute__ ((packed));

void com_read_data_all_wait(uint8_t *buf, size_t size);
int com_read_data(uint8_t *buf, size_t size);
int com_write_data(const uint8_t *buf, size_t size);
int com_send_ack(bool err);
int com_wait_ack(void);
int com_download_img(struct com_data *com, uint8_t *buf, uint32_t size, uint32_t index);
int com_get_version(void);

extern struct device *flash_dev;

#endif