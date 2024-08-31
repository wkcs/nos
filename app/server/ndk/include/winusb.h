/**
 * Copyright (C) 2024-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NDK_WINUSB_H__
#define __NDK_WINUSB_H__

#include <kernel/sem.h>
#include <kernel/types.h>
#include <kernel/device.h>

struct ndk_usb {
    struct device *dev;
    sem_t ctrl_sem;
    uint8_t *ctrl_msg_buf;
    uint32_t ctrl_msg_size;
};

struct ndk_usb *ndk_usb_alloc(void);
void ndk_usb_free(struct ndk_usb *usb);
ssize_t ndk_usb_read(struct ndk_usb *usb, uint8_t *buf, size_t size);
ssize_t ndk_usb_write(struct ndk_usb *usb, uint8_t *buf, size_t size);
ssize_t ndk_usb_read_timeout(struct ndk_usb *usb, uint8_t *buf, size_t size);
ssize_t ndk_usb_write_timeout(struct ndk_usb *usb, uint8_t *buf, size_t size);

#endif /* __NDK_WINUSB_H__ */
