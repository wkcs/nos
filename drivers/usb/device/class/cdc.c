/**
 * Copyright (C) 2024-2026 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[USB_CDC]: " fmt

#include <kernel/kernel.h>
#include <kernel/task.h>
#include <kernel/sleep.h>
#include <kernel/irq.h>
#include <kernel/spinlock.h>
#include <kernel/device.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/init.h>
#include <kernel/clk.h>
#include <nos/completion.h>
#include <nos/timeout.h>
#include <string.h>
#include <lib/kfifo.h>
#include <drivers/usb/usb_common.h>
#include <drivers/usb/usb_device.h>
#ifdef CONFIG_TTY
#include <nos/tty.h>
#endif
#ifdef CONFIG_USB_CDC_LOG_BACKEND
#include <kernel/log_backend.h>
#endif

#include "cdc.h"

#ifdef CONFIG_VCOM_TX_TIMEOUT
#define VCOM_TX_TIMEOUT       CONFIG_VCOM_TX_TIMEOUT
#else
#define VCOM_TX_TIMEOUT       1000U
#endif

#ifdef CONFIG_CDC_RX_BUFSIZE
#define CDC_RX_BUFSIZE        CONFIG_CDC_RX_BUFSIZE
#else
#define CDC_RX_BUFSIZE        128U
#endif

#define CDC_MAX_PACKET_SIZE   64U
#define CDC_TX_BUFSIZE        1024U
#define VCOM_LOG_POLL_MS      20U
#define VCOM_TASK_STACK_SIZE  768U

#ifdef CONFIG_VCOM_SERNO
#define _SER_NO               CONFIG_VCOM_SERNO
#else
#define _SER_NO               "32021919830108"
#endif

#ifdef CONFIG_VCOM_SER_LEN
#define _SER_NO_LEN           CONFIG_VCOM_SER_LEN
#else
#define _SER_NO_LEN           14
#endif

#ifndef kfifo_avail
#define kfifo_avail(f) kfifo_unused(f)
#endif

struct vcom {
    struct device dev;
#ifdef CONFIG_TTY
    struct nos_tty tty;
#endif
#ifdef CONFIG_USB_CDC_LOG_BACKEND
    struct kernel_log_backend log_backend;
    bool log_registered;
    unsigned int log_dropped;
#endif
    struct task_struct *cdc_task;
    struct ufunction *func;
    uep_t ep_out;
    uep_t ep_in;
    uep_t ep_cmd;
    struct nos_completion wait;
    struct nos_completion tx_ready;
    struct nos_completion rx_ready;
    spinlock_t rx_lock;
    spinlock_t tx_lock;
    bool initialized;
    bool connected;
    bool line_dtr;
    bool line_rts;
    bool rx_active;
    bool rx_paused;
    bool tx_in_flight;
    bool tx_zlp_pending;
    uint32_t session_gen;
    uint8_t ep_rx_buf[CDC_MAX_PACKET_SIZE];
    uint8_t ep_tx_buf[CDC_MAX_PACKET_SIZE];
    uint8_t rx_fifo_buf[CDC_RX_BUFSIZE];
    struct kfifo rx_fifo;
    uint8_t tx_fifo_buf[CDC_TX_BUFSIZE];
    struct kfifo tx_fifo;
};

static struct vcom s_vcom;

/* 持久存储串口线路编码，供 EP0 GET/SET_LINE_CODING 直接使用 */
__aligned(4)
static struct ucdc_line_coding line_coding = {
    .dwDTERate   = 115200U,
    .bCharFormat = 0U,
    .bParityType = 0U,
    .bDataBits   = 8U,
};

#ifndef CONFIG_USB_DEVICE_COMPOSITE
__aligned(4)
static struct udevice_descriptor dev_desc = {
    USB_DESC_LENGTH_DEVICE,     /* bLength */
    USB_DESC_TYPE_DEVICE,       /* type */
    USB_BCD_VERSION,            /* bcdUSB */
    USB_CLASS_CDC,              /* bDeviceClass */
    0x02,                       /* bDeviceSubClass */
    0x00,                       /* bDeviceProtocol */
    CDC_MAX_PACKET_SIZE,        /* bMaxPacketSize0 */
    _VENDOR_ID,                 /* idVendor */
    _PRODUCT_ID,                /* idProduct */
    USB_BCD_DEVICE,             /* bcdDevice */
    USB_STRING_MANU_INDEX,      /* iManufacturer */
    USB_STRING_PRODUCT_INDEX,   /* iProduct */
    USB_STRING_SERIAL_INDEX,    /* iSerialNumber */
    USB_DYNAMIC,                /* bNumConfigurations */
};

__aligned(4)
static struct usb_qualifier_descriptor dev_qualifier = {
    sizeof(dev_qualifier),          /* bLength */
    USB_DESC_TYPE_DEVICEQUALIFIER,  /* bDescriptorType */
    0x0200,                         /* bcdUSB */
    USB_CLASS_CDC,                  /* bDeviceClass */
    0x02,                           /* bDeviceSubClass */
    0x00,                           /* bDeviceProtocol */
    64,                             /* bMaxPacketSize0 */
    0x01,                           /* bNumConfigurations */
    0,
};

__aligned(4)
static char serno[_SER_NO_LEN + 1] = { '\0' };

__weak int vcom_get_stored_serno(char *out_serno, int size);

int vcom_get_stored_serno(char *out_serno, int size)
{
    (void)out_serno;
    (void)size;
    return -EFAULT;
}

__aligned(4)
static const char *_ustring[] = {
    "Language",
    "wkcs",
    "NOS Virtual Serial",
    serno,
    "Configuration",
    "Interface",
};
#endif /* !CONFIG_USB_DEVICE_COMPOSITE */

/* CDC 通信接口描述符 */
__aligned(4)
static const struct ucdc_comm_descriptor _comm_desc = {
#ifdef CONFIG_USB_DEVICE_COMPOSITE
    {
        USB_DESC_LENGTH_IAD,
        USB_DESC_TYPE_IAD,
        USB_DYNAMIC,
        0x02,
        USB_CDC_CLASS_COMM,
        USB_CDC_SUBCLASS_ACM,
        USB_CDC_PROTOCOL_V25TER,
        0x00,
    },
#endif
    {
        USB_DESC_LENGTH_INTERFACE,
        USB_DESC_TYPE_INTERFACE,
        USB_DYNAMIC,
        0x00,
        0x01,
        USB_CDC_CLASS_COMM,
        USB_CDC_SUBCLASS_ACM,
        USB_CDC_PROTOCOL_V25TER,
        0,
    },
    {
        0x05,
        USB_CDC_CS_INTERFACE,
        USB_CDC_SCS_HEADER,
        0x0110,
    },
    {
        0x05,
        USB_CDC_CS_INTERFACE,
        USB_CDC_SCS_CALL_MGMT,
        0x00,
        USB_DYNAMIC,
    },
    {
        0x04,
        USB_CDC_CS_INTERFACE,
        USB_CDC_SCS_ACM,
        0x02,
    },
    {
        0x05,
        USB_CDC_CS_INTERFACE,
        USB_CDC_SCS_UNION,
        USB_DYNAMIC,
        USB_DYNAMIC,
    },
    {
        USB_DESC_LENGTH_ENDPOINT,
        USB_DESC_TYPE_ENDPOINT,
        USB_DYNAMIC | USB_DIR_IN,
        USB_EP_ATTR_INT,
        0x08,
        0xFF,
    },
};

/* CDC 数据接口描述符 */
__aligned(4)
static const struct ucdc_data_descriptor _data_desc = {
    {
        USB_DESC_LENGTH_INTERFACE,
        USB_DESC_TYPE_INTERFACE,
        USB_DYNAMIC,
        0x00,
        0x02,
        USB_CDC_CLASS_DATA,
        0x00,
        0x00,
        0x00,
    },
    {
        USB_DESC_LENGTH_ENDPOINT,
        USB_DESC_TYPE_ENDPOINT,
        USB_DYNAMIC | USB_DIR_OUT,
        USB_EP_ATTR_BULK,
        USB_CDC_BUFSIZE,
        0x00,
    },
    {
        USB_DESC_LENGTH_ENDPOINT,
        USB_DESC_TYPE_ENDPOINT,
        USB_DYNAMIC | USB_DIR_IN,
        USB_EP_ATTR_BULK,
        USB_CDC_BUFSIZE,
        0x00,
    },
};

static bool vcom_is_connected(const struct vcom *data)
{
    udevice_state_t state;

    if (data == NULL || !data->connected ||
        data->func == NULL || data->func->device == NULL) {
        return false;
    }
    /* core.c 在 SET_CONFIGURATION 中先调用 FUNC_ENABLE 再置 USB_STATE_CONFIGURED */
    state = data->func->device->state;
    return state == USB_STATE_CONFIGURED || state == USB_STATE_ADDRESS;
}

static bool vcom_is_session_active(const struct vcom *data)
{
    return vcom_is_connected(data) && (data->line_dtr || data->rx_active);
}

static uint16_t vcom_ep_maxpacket(const uep_t ep)
{
    uint16_t mps = (ep != NULL && ep->ep_desc != NULL) ? EP_MAXPACKET(ep) : 0U;

    if (mps == 0U || mps > CDC_MAX_PACKET_SIZE) {
        return CDC_MAX_PACKET_SIZE;
    }
    return mps;
}

/* 在锁外挂载 Bulk OUT 接收请求 */
static void vcom_arm_rx(struct vcom *data)
{
    uint16_t mps;

    if (!vcom_is_connected(data) || data->ep_out == NULL) {
        return;
    }
    mps = vcom_ep_maxpacket(data->ep_out);
    data->ep_out->buffer = data->ep_rx_buf;
    data->ep_out->request.buffer = data->ep_rx_buf;
    data->ep_out->request.size = mps;
    data->ep_out->request.req_type = UIO_REQUEST_READ_BEST;
    usbd_io_request(data->func->device, data->ep_out, &data->ep_out->request);
}

#ifdef CONFIG_TTY
static int vcom_tty_read(void *context, unsigned char *buf, unsigned int len, bool wait);
static int vcom_tty_write(void *context, const unsigned char *buf, unsigned int len);
static bool vcom_tty_poll_session(void *context, uint32_t *generation, bool *non_interactive);
static void vcom_tty_on_exit(void *context, int32_t exit_code);

static const struct nos_tty_ops vcom_tty_ops = {
    .read = vcom_tty_read,
    .write = vcom_tty_write,
    .poll_session = vcom_tty_poll_session,
    .on_exit = vcom_tty_on_exit,
};
#endif

/* 幂等初始化静态 VCOM 实例，确保早于 USB 装配的调用方也可安全拿到 TTY */
static int vcom_ensure_initialized(struct vcom *data)
{
    if (data == NULL) {
        return -EINVAL;
    }
    if (data->initialized) {
        return 0;
    }

    spin_lock_init(&data->rx_lock);
    spin_lock_init(&data->tx_lock);

    if (kfifo_init(&data->rx_fifo, data->rx_fifo_buf, CDC_RX_BUFSIZE, 1) < 0) {
        return -ENOMEM;
    }
    if (kfifo_init(&data->tx_fifo, data->tx_fifo_buf, CDC_TX_BUFSIZE, 1) < 0) {
        return -ENOMEM;
    }

    nos_completion_init(&data->wait);
    nos_completion_init(&data->tx_ready);
    nos_completion_init(&data->rx_ready);

#ifdef CONFIG_TTY
    if (nos_tty_init(&data->tty, &vcom_tty_ops, data) != 0) {
        return -EINVAL;
    }
#ifdef CONFIG_CONSOLE_CRLF
    data->tty.termios.c_oflag |= ONLCR;
#endif
#endif

    data->initialized = true;
    return 0;
}

static void _vcom_reset_state(struct vcom *data, bool clear_tx)
{
    addr_t flags;

    flags = disable_irq_save();
    spin_lock(&data->rx_lock);
    data->connected = false;
    data->line_dtr = false;
    data->line_rts = false;
    data->rx_active = false;
    data->rx_paused = false;
    data->session_gen++;
    kfifo_skip(&data->rx_fifo, kfifo_used(&data->rx_fifo));
    spin_unlock(&data->rx_lock);
    enable_irq_save(flags);

    flags = disable_irq_save();
    spin_lock(&data->tx_lock);
    data->tx_in_flight = false;
    data->tx_zlp_pending = false;
    if (clear_tx) {
        kfifo_skip(&data->tx_fifo, kfifo_used(&data->tx_fifo));
    }
    spin_unlock(&data->tx_lock);
    enable_irq_save(flags);
}

static int _ep_in_handler(ufunction_t func, size_t size)
{
    struct vcom *data;
    addr_t flags;
    bool send_zlp = false;

    (void)size;
    if (func == NULL || func->user_data == NULL) {
        return -EINVAL;
    }
    data = (struct vcom *)func->user_data;

    flags = disable_irq_save();
    spin_lock(&data->tx_lock);
    if (vcom_is_connected(data) && data->tx_zlp_pending) {
        data->tx_zlp_pending = false;
        send_zlp = true;
    } else {
        data->tx_zlp_pending = false;
        data->tx_in_flight = false;
    }
    spin_unlock(&data->tx_lock);
    enable_irq_save(flags);

    if (send_zlp) {
        data->ep_in->request.buffer = NULL;
        data->ep_in->request.size = 0;
        data->ep_in->request.req_type = UIO_REQUEST_WRITE;
        usbd_io_request(func->device, data->ep_in, &data->ep_in->request);
        return 0;
    }

    nos_complete(&data->wait);
    return 0;
}

static int _ep_out_handler(ufunction_t func, size_t size)
{
    struct vcom *data;
    addr_t flags;
    bool arm_rx = false;
    uint16_t mps;

    if (func == NULL || func->user_data == NULL) {
        return -EINVAL;
    }
    data = (struct vcom *)func->user_data;
    mps = vcom_ep_maxpacket(data->ep_out);

    flags = disable_irq_save();
    spin_lock(&data->rx_lock);
    if (size > 0U && size <= sizeof(data->ep_rx_buf)) {
        kfifo_in(&data->rx_fifo, data->ep_rx_buf, (unsigned int)size);
        if (!data->line_dtr && !data->rx_active) {
            data->rx_active = true;
            data->session_gen++;
        }
    }
    if (vcom_is_connected(data) && kfifo_avail(&data->rx_fifo) >= mps) {
        data->rx_paused = false;
        arm_rx = true;
    } else {
        /* FIFO 空间不足时暂停挂载接收，硬件侧自动对主机回复 NAK 形成背压 */
        data->rx_paused = true;
    }
    spin_unlock(&data->rx_lock);
    enable_irq_save(flags);

    if (size > 0U) {
        nos_complete(&data->rx_ready);
    }
    if (arm_rx) {
        vcom_arm_rx(data);
    }

    return 0;
}

static int _ep_cmd_handler(ufunction_t func, size_t size)
{
    (void)size;
    if (func == NULL) {
        return -EINVAL;
    }
    return 0;
}

static int _cdc_get_line_coding(udevice_t device, ureq_t setup)
{
    uint16_t size;

    if (device == NULL || setup == NULL) {
        return -EINVAL;
    }

    size = setup->wLength > sizeof(line_coding) ? (uint16_t)sizeof(line_coding) : setup->wLength;
    usbd_ep0_write(device, (void *)&line_coding, size);
    return 0;
}

static int _cdc_set_line_coding_callback(udevice_t device, size_t size)
{
    (void)size;
    if (device == NULL || device->dcd == NULL) {
        return -EINVAL;
    }
    dcd_ep0_send_status(device->dcd);
    return 0;
}

static int _cdc_set_line_coding(udevice_t device, ureq_t setup)
{
    uint16_t size;

    if (device == NULL || setup == NULL) {
        return -EINVAL;
    }

    size = setup->wLength > sizeof(line_coding) ? (uint16_t)sizeof(line_coding) : setup->wLength;
    usbd_ep0_read(device, (void *)&line_coding, size, _cdc_set_line_coding_callback);
    return 0;
}

static int _interface_handler(ufunction_t func, ureq_t setup)
{
    struct vcom *data;
    addr_t flags;
    bool dtr;
    bool rts;
    bool prev_dtr;
    bool wake_reader = false;

    if (func == NULL || func->device == NULL || func->user_data == NULL || setup == NULL) {
        return -EINVAL;
    }
    data = (struct vcom *)func->user_data;

    switch (setup->bRequest) {
    case CDC_SEND_ENCAPSULATED_COMMAND:
    case CDC_GET_ENCAPSULATED_RESPONSE:
    case CDC_SET_COMM_FEATURE:
    case CDC_GET_COMM_FEATURE:
    case CDC_CLEAR_COMM_FEATURE:
    case CDC_SEND_BREAK:
        dcd_ep0_send_status(func->device->dcd);
        break;
    case CDC_SET_LINE_CODING:
        return _cdc_set_line_coding(func->device, setup);
    case CDC_GET_LINE_CODING:
        return _cdc_get_line_coding(func->device, setup);
    case CDC_SET_CONTROL_LINE_STATE:
        dtr = (setup->wValue & 0x01U) != 0U;
        rts = (setup->wValue & 0x02U) != 0U;

        flags = disable_irq_save();
        spin_lock(&data->rx_lock);
        prev_dtr = data->line_dtr;
        data->line_dtr = dtr;
        data->line_rts = rts;
        if (dtr != prev_dtr) {
            data->rx_active = false;
            data->session_gen++;
            wake_reader = !dtr;
        }
        spin_unlock(&data->rx_lock);
        enable_irq_save(flags);

        if (wake_reader) {
            nos_complete_all(&data->rx_ready);
        }
        dcd_ep0_send_status(func->device->dcd);
        break;
    default:
        return -EINVAL;
    }

    return 0;
}

static int _function_enable(ufunction_t func)
{
    struct vcom *data;
    addr_t flags;

    if (func == NULL || func->user_data == NULL) {
        return -EINVAL;
    }
    data = (struct vcom *)func->user_data;

    /* 保留断连/启动期尚未发出的日志 TX FIFO，仅清理 RX 与旧传输状态 */
    _vcom_reset_state(data, false);
    nos_completion_reset(&data->rx_ready);
    nos_completion_reset(&data->wait);

    flags = disable_irq_save();
    spin_lock(&data->rx_lock);
    data->connected = true;
    data->rx_paused = false;
    spin_unlock(&data->rx_lock);
    enable_irq_save(flags);

    vcom_arm_rx(data);

    /* 无条件唤醒发送任务，使其从无限休眠切换为已连接态的周期性日志聚合同步 */
    nos_complete(&data->tx_ready);

    return 0;
}

static int _function_disable(ufunction_t func)
{
    struct vcom *data;

    if (func == NULL || func->user_data == NULL) {
        return -EINVAL;
    }
    data = (struct vcom *)func->user_data;

    _vcom_reset_state(data, true);
    nos_complete_all(&data->rx_ready);
    nos_complete_all(&data->wait);

    return 0;
}

static struct ufunction_ops ops = {
    _function_enable,
    _function_disable,
    NULL,
};

static int _cdc_descriptor_config(ucdc_comm_desc_t comm, uint8_t cintf_nr,
                                  ucdc_data_desc_t data, uint8_t dintf_nr)
{
    comm->call_mgmt_desc.data_interface = dintf_nr;
    comm->union_desc.master_interface = cintf_nr;
    comm->union_desc.slave_interface0 = dintf_nr;
#ifdef CONFIG_USB_DEVICE_COMPOSITE
    comm->iad_desc.bFirstInterface = cintf_nr;
#endif
    return 0;
}

static ssize_t vcom_read_common(struct vcom *data, void *buffer, size_t size,
                                uint32_t timeout_tick, bool require_session)
{
    addr_t flags;
    unsigned int read_size;
    bool arm_rx = false;

    if (data == NULL || buffer == NULL || size == 0U) {
        return -EINVAL;
    }

    for (;;) {
        bool ready = false;
        bool active = false;
        int rc;

        flags = disable_irq_save();
        spin_lock(&data->rx_lock);
        active = require_session ? vcom_is_session_active(data) : vcom_is_connected(data);
        if (active && kfifo_used(&data->rx_fifo) > 0U) {
            ready = true;
        }
        spin_unlock(&data->rx_lock);
        enable_irq_save(flags);

        if (!active) {
            return -ENOTCONN;
        }
        if (ready) {
            break;
        }
        if (timeout_tick == 0U) {
            return -EAGAIN;
        }

        rc = nos_completion_wait(&data->rx_ready, timeout_tick);
        if (rc < 0) {
            flags = disable_irq_save();
            spin_lock(&data->rx_lock);
            active = require_session ? vcom_is_session_active(data) : vcom_is_connected(data);
            ready = active && kfifo_used(&data->rx_fifo) > 0U;
            spin_unlock(&data->rx_lock);
            enable_irq_save(flags);

            if (!active) {
                return -ENOTCONN;
            }
            if (!ready) {
                return -ETIMEDOUT;
            }
            break;
        }
    }

    flags = disable_irq_save();
    spin_lock(&data->rx_lock);
    read_size = kfifo_out(&data->rx_fifo, buffer, (unsigned int)size);
    if (vcom_is_connected(data) && data->rx_paused &&
        kfifo_avail(&data->rx_fifo) >= vcom_ep_maxpacket(data->ep_out)) {
        data->rx_paused = false;
        arm_rx = true;
    }
    spin_unlock(&data->rx_lock);
    enable_irq_save(flags);

    if (arm_rx) {
        vcom_arm_rx(data);
    }

    return (ssize_t)read_size;
}

static ssize_t vcom_read(struct device *dev, addr_t pos, void *buffer, size_t size)
{
    struct vcom *data = container_of(dev, struct vcom, dev);

    (void)pos;
    return vcom_read_common(data, buffer, size, NOS_WAIT_FOREVER, false);
}

static ssize_t vcom_read_timeout(struct device *dev, addr_t pos,
                                 void *buffer, size_t size, uint32_t tick)
{
    struct vcom *data = container_of(dev, struct vcom, dev);

    (void)pos;
    return vcom_read_common(data, buffer, size, tick, false);
}

static ssize_t vcom_write_internal(struct vcom *data, const void *buffer,
                                   size_t size, bool atomic_chunk)
{
    addr_t flags;
    unsigned int avail;
    unsigned int written = 0U;

    if (data == NULL || buffer == NULL || size == 0U) {
        return -EINVAL;
    }

    flags = disable_irq_save();
    spin_lock(&data->tx_lock);
    if (!vcom_is_connected(data)) {
        spin_unlock(&data->tx_lock);
        enable_irq_save(flags);
        return -ENOTCONN;
    }
    avail = kfifo_avail(&data->tx_fifo);
    if (avail == 0U || (atomic_chunk && avail < size)) {
        spin_unlock(&data->tx_lock);
        enable_irq_save(flags);
        return -EAGAIN;
    }
    written = kfifo_in(&data->tx_fifo, buffer, (unsigned int)size);
    spin_unlock(&data->tx_lock);
    enable_irq_save(flags);

    if (written == 0U) {
        return -EAGAIN;
    }

    nos_complete(&data->tx_ready);
    return (ssize_t)written;
}

static ssize_t vcom_write(struct device *dev, addr_t pos, const void *buffer, size_t size)
{
    struct vcom *data = container_of(dev, struct vcom, dev);

    (void)pos;
    return vcom_write_internal(data, buffer, size, false);
}

#ifdef CONFIG_TTY
static int vcom_tty_read(void *context, unsigned char *buf, unsigned int len, bool wait)
{
    struct vcom *data = (struct vcom *)context;
    ssize_t rc = vcom_read_common(data, buf, (size_t)len,
                                  wait ? NOS_WAIT_FOREVER : 0U, true);

    return (int)rc;
}

static int vcom_tty_write(void *context, const unsigned char *buf, unsigned int len)
{
    struct vcom *data = (struct vcom *)context;

    /* 当写入小块 ONLCR 输出时保证整块原子入队，防止半个 CRLF 序列丢失 */
    return (int)vcom_write_internal(data, buf, (size_t)len, len <= 32U);
}

static bool vcom_tty_poll_session(void *context, uint32_t *generation, bool *non_interactive)
{
    struct vcom *data = (struct vcom *)context;
    addr_t flags;
    bool active;

    if (data == NULL) {
        return false;
    }

    flags = disable_irq_save();
    spin_lock(&data->rx_lock);
    active = vcom_is_session_active(data);
    if (generation != NULL) {
        *generation = data->session_gen;
    }
    spin_unlock(&data->rx_lock);
    enable_irq_save(flags);

    if (non_interactive != NULL) {
        *non_interactive = false;
    }
    return active;
}

static void vcom_tty_on_exit(void *context, int32_t exit_code)
{
    struct vcom *data = (struct vcom *)context;
    addr_t flags;

    (void)exit_code;
    if (data == NULL) {
        return;
    }

    flags = disable_irq_save();
    spin_lock(&data->rx_lock);
    data->rx_active = false;
    data->session_gen++;
    spin_unlock(&data->rx_lock);
    enable_irq_save(flags);

    nos_complete_all(&data->rx_ready);
}
#endif /* CONFIG_TTY */

struct nos_tty *usbd_vcom_tty(void)
{
#ifdef CONFIG_TTY
    if (vcom_ensure_initialized(&s_vcom) != 0) {
        return NULL;
    }
    return &s_vcom.tty;
#else
    return NULL;
#endif
}

#ifdef CONFIG_USB_CDC_LOG_BACKEND
static unsigned int vcom_calc_wire_len(const uint8_t *buf, unsigned int len)
{
#ifdef CONFIG_CONSOLE_CRLF
    unsigned int wire_len = len;
    unsigned int i;

    for (i = 0U; i < len; i++) {
        if (buf[i] == '\n' && (i == 0U || buf[i - 1U] != '\r')) {
            wire_len++;
        }
    }
    return wire_len;
#else
    (void)buf;
    return len;
#endif
}

static void vcom_fifo_write_crlf_locked(struct kfifo *fifo, const uint8_t *buf, unsigned int len)
{
#ifdef CONFIG_CONSOLE_CRLF
    unsigned int pos = 0U;

    while (pos < len) {
        unsigned int end = pos;

        while (end < len && buf[end] != '\n') {
            end++;
        }
        if (end > pos) {
            kfifo_in(fifo, buf + pos, end - pos);
            pos = end;
        }
        if (pos < len) {
            if (pos > 0U && buf[pos - 1U] == '\r') {
                kfifo_in(fifo, buf + pos, 1U);
            } else {
                static const uint8_t crlf[2] = { '\r', '\n' };
                kfifo_in(fifo, crlf, 2U);
            }
            pos++;
        }
    }
#else
    kfifo_in(fifo, buf, len);
#endif
}

static int vcom_log_write(const struct kernel_log_record *record, const void *buf, void *priv)
{
    struct vcom *data = (struct vcom *)priv;
    const uint8_t *src = (const uint8_t *)buf;
    unsigned int wire_len;
    unsigned int avail;
    addr_t flags;
    bool session_open;

    if (data == NULL || record == NULL || src == NULL || record->length == 0U) {
        return 0;
    }

    wire_len = vcom_calc_wire_len(src, record->length);
    if (wire_len > CDC_TX_BUFSIZE) {
        return 0;
    }

    /* 禁止在 log_backend 上下文中调用 nos_complete 或打印日志，防止锁序反转和自激死循环 */
    session_open = __atomic_load_n(&data->connected, __ATOMIC_ACQUIRE) &&
                   (__atomic_load_n(&data->line_dtr, __ATOMIC_ACQUIRE) ||
                    __atomic_load_n(&data->rx_active, __ATOMIC_ACQUIRE));

    flags = disable_irq_save();
    spin_lock(&data->tx_lock);
    avail = kfifo_avail(&data->tx_fifo);
    if (avail < wire_len) {
        if (!session_open) {
            /* 终端未打开时覆盖最旧日志，始终保留最新日志 */
            kfifo_skip(&data->tx_fifo, wire_len - avail);
        } else {
            data->log_dropped++;
            spin_unlock(&data->tx_lock);
            enable_irq_save(flags);
            return 0;
        }
    }
    vcom_fifo_write_crlf_locked(&data->tx_fifo, src, record->length);
    spin_unlock(&data->tx_lock);
    enable_irq_save(flags);

    return (int)record->length;
}
#endif /* CONFIG_USB_CDC_LOG_BACKEND */

static void vcom_tx_task_entry(void *parameter)
{
    struct vcom *data = (struct vcom *)parameter;

    for (;;) {
        bool connected = __atomic_load_n(&data->connected, __ATOMIC_ACQUIRE);

        /* 已连接时周期性唤醒以聚合发送无锁入队的内核日志，未连接时零功耗挂起 */
        nos_completion_wait(&data->tx_ready,
                            connected ? msec_to_tick(VCOM_LOG_POLL_MS) : NOS_WAIT_FOREVER);

        for (;;) {
            addr_t flags;
            unsigned int len = 0U;
            bool need_wait = false;

            flags = disable_irq_save();
            spin_lock(&data->tx_lock);
            if (!vcom_is_connected(data)) {
                spin_unlock(&data->tx_lock);
                enable_irq_save(flags);
                break;
            }
            if (data->tx_in_flight) {
                need_wait = true;
            } else if (kfifo_used(&data->tx_fifo) > 0U) {
                uint16_t mps = vcom_ep_maxpacket(data->ep_in);
                len = kfifo_out(&data->tx_fifo, data->ep_tx_buf, mps);
                if (len > 0U) {
                    data->tx_in_flight = true;
                    data->tx_zlp_pending = ((len % mps) == 0U);
                }
            }
            spin_unlock(&data->tx_lock);
            enable_irq_save(flags);

            if (need_wait) {
                /* 上一包仍在硬件 IN FIFO 等待主机读取，挂起至完成或设备断开 */
                nos_completion_wait(&data->wait, NOS_WAIT_FOREVER);
                continue;
            }
            if (len == 0U) {
                break;
            }

            nos_completion_reset(&data->wait);
            data->ep_in->request.buffer = data->ep_tx_buf;
            data->ep_in->request.size = len;
            data->ep_in->request.req_type = UIO_REQUEST_WRITE;
            usbd_io_request(data->func->device, data->ep_in, &data->ep_in->request);

            nos_completion_wait(&data->wait, NOS_WAIT_FOREVER);
        }
    }
}

static int usb_vcom_init(struct ufunction *func)
{
    struct vcom *data = (struct vcom *)func->user_data;
    int rc;

    rc = vcom_ensure_initialized(data);
    if (rc != 0) {
        pr_err("init state failed: %d\n", rc);
        return rc;
    }

    if (data->dev.name == NULL) {
        device_init(&data->dev);
        data->dev.name = "vcom";
        data->dev.ops.read = vcom_read;
        data->dev.ops.write = vcom_write;
        data->dev.ops.read_timeout = vcom_read_timeout;
        device_register(&data->dev);
    }

    if (data->cdc_task == NULL) {
        data->cdc_task = task_create("vcom", vcom_tx_task_entry, data,
                                     5, VCOM_TASK_STACK_SIZE, 5, NULL);
        if (data->cdc_task == NULL) {
            pr_err("create vcom tx task failed\n");
            return -ENOMEM;
        }
        task_ready(data->cdc_task);
    }

#ifdef CONFIG_USB_CDC_LOG_BACKEND
    if (!data->log_registered) {
        data->log_backend.write = vcom_log_write;
        data->log_backend.priv = data;
        data->log_backend.next = NULL;
        if (kernel_log_backend_register(&data->log_backend) == 0) {
            data->log_registered = true;
#ifdef CONFIG_LOG_DEFAULT_VCOM
            kernel_log_backend_unregister(kernel_log_console_backend());
#endif
        }
    }
#endif

    return 0;
}

ufunction_t usbd_function_cdc_create(udevice_t device)
{
    ufunction_t func;
    struct vcom *data = &s_vcom;
    uintf_t intf_comm, intf_data;
    ualtsetting_t comm_setting, data_setting;
    ucdc_data_desc_t data_desc;
    ucdc_comm_desc_t comm_desc;

    if (device == NULL) {
        pr_err("device is NULL\n");
        return NULL;
    }

#ifndef CONFIG_USB_DEVICE_COMPOSITE
    memset(serno, 0, sizeof(serno));
    if (vcom_get_stored_serno(serno, _SER_NO_LEN) != 0) {
        memset(serno, 0, sizeof(serno));
        memcpy(serno, _SER_NO, strlen(_SER_NO));
    }
    usbd_device_set_string(device, _ustring);
    func = usbd_function_new(device, &dev_desc, &ops);
    if (func != NULL && device->dcd != NULL && device->dcd->device_is_hs) {
        usbd_device_set_qualifier(device, &dev_qualifier);
    }
#else
    func = usbd_function_new(device, NULL, &ops);
#endif
    if (func == NULL) {
        pr_err("alloc cdc function failed\n");
        return NULL;
    }

    func->user_data = data;
    data->func = func;

    if (usb_vcom_init(func) != 0) {
        return NULL;
    }

    intf_comm = usbd_interface_new(device, _interface_handler);
    intf_data = usbd_interface_new(device, _interface_handler);
    comm_setting = usbd_altsetting_new(sizeof(struct ucdc_comm_descriptor));
    data_setting = usbd_altsetting_new(sizeof(struct ucdc_data_descriptor));
    if (intf_comm == NULL || intf_data == NULL ||
        comm_setting == NULL || data_setting == NULL) {
        pr_err("alloc cdc descriptor failed\n");
        return NULL;
    }

    usbd_altsetting_config_descriptor(comm_setting, &_comm_desc,
                                      (addr_t)&((ucdc_comm_desc_t)0)->intf_desc);
    usbd_altsetting_config_descriptor(data_setting, &_data_desc, 0);
    _cdc_descriptor_config(comm_setting->desc, intf_comm->intf_num,
                           data_setting->desc, intf_data->intf_num);

    comm_desc = (ucdc_comm_desc_t)comm_setting->desc;
    data->ep_cmd = usbd_endpoint_new(&comm_desc->ep_desc, _ep_cmd_handler);
    usbd_altsetting_add_endpoint(comm_setting, data->ep_cmd);
    usbd_interface_add_altsetting(intf_comm, comm_setting);
    usbd_set_altsetting(intf_comm, 0);
    usbd_function_add_interface(func, intf_comm);

    data_desc = (ucdc_data_desc_t)data_setting->desc;
    data->ep_out = usbd_endpoint_new(&data_desc->ep_out_desc, _ep_out_handler);
    data->ep_in = usbd_endpoint_new(&data_desc->ep_in_desc, _ep_in_handler);
    usbd_altsetting_add_endpoint(data_setting, data->ep_in);
    usbd_altsetting_add_endpoint(data_setting, data->ep_out);
    usbd_interface_add_altsetting(intf_data, data_setting);
    usbd_set_altsetting(intf_data, 0);
    usbd_function_add_interface(func, intf_data);

    return func;
}

static struct udclass vcom_class = {
    .usbd_function_create = usbd_function_cdc_create,
};

int usbd_vcom_class_register(void)
{
    INIT_LIST_HEAD(&vcom_class.list);
    usbd_class_register(&vcom_class);
    return 0;
}
