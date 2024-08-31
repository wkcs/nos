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

#include "include/winusb.h"

struct ndk_usb *g_ndk_usb;

#define USB_TIMEDOUT_MS 1000

static void ndk_winusb_ctrl_callback(uint8_t *buf, size_t size)
{
    int i;

    g_ndk_usb->ctrl_msg_buf = buf;
    g_ndk_usb->ctrl_msg_size = size;
    sem_send_one(&g_ndk_usb->ctrl_sem);
}

struct ndk_usb *ndk_usb_alloc(void)
{
    int rc;
    struct ndk_usb *ndk_usb;
    struct device *dev;
    int i;

    for (i = 0; i < 50; i++) {
        dev = device_find_by_name("winusb");
        if (dev != NULL) {
            break;
        }
    }
    if (!dev) {
        pr_err("winusb device not found\r\n");
        return NULL;
    }

    ndk_usb = kzalloc(sizeof(struct ndk_usb), GFP_KERNEL);
    if (!ndk_usb) {
        pr_err("ndk_usb malloc failed\r\n");
        return NULL;
    }
    ndk_usb->dev = dev;
    dev->ops.control(dev, 0x03, ndk_winusb_ctrl_callback);
    g_ndk_usb = ndk_usb;

    sem_init(&ndk_usb->ctrl_sem, 0);

    return ndk_usb;
}

void ndk_usb_free(struct ndk_usb *usb)
{
    if (usb == NULL) {
        return;
    }
    kfree(usb);
}

ssize_t ndk_usb_read(struct ndk_usb *usb, uint8_t *buf, size_t size)
{
    int rc;

    if (buf == NULL) {
        pr_err("buf is null\r\n");
        return -EINVAL;
    }

    rc = usb->dev->ops.read(usb->dev, 0, buf, size);
    return rc;
}

ssize_t ndk_usb_write(struct ndk_usb *usb, uint8_t *buf, size_t size)
{
    int rc;

    if (buf == NULL) {
        pr_err("buf is null\r\n");
        return -EINVAL;
    }

    rc = usb->dev->ops.write(usb->dev, 0, buf, size);
    return rc;
}

ssize_t ndk_usb_read_timeout(struct ndk_usb *usb, uint8_t *buf, size_t size)
{
    int rc;

    if (buf == NULL) {
        pr_err("buf is null\r\n");
        return -EINVAL;
    }

    rc = usb->dev->ops.read_timeout(usb->dev, 0, buf, size, msec_to_tick(USB_TIMEDOUT_MS));
    return rc;
}

ssize_t ndk_usb_write_timeout(struct ndk_usb *usb, uint8_t *buf, size_t size)
{
    int rc;

    if (buf == NULL) {
        pr_err("buf is null\r\n");
        return -EINVAL;
    }

    rc = usb->dev->ops.write_timeout(usb->dev, 0, buf, size, msec_to_tick(USB_TIMEDOUT_MS));
    return rc;
}
