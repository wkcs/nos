/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * 验证 USB CDC VCOM 日志后端、TTY 桥接、EP 状态机与无回显测试代码回归行为
 */

#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <kernel/kernel.h>
#include <kernel/irq.h>
#include <kernel/mm.h>
#include <kernel/printk.h>
#include <kernel/task.h>
#include <kernel/sleep.h>
#include <nos/completion.h>
#include <nos/tty.h>
#include <drivers/usb/usb_common.h>
#include <drivers/usb/usb_device.h>
#include "cdc.h"

addr_t kernel_running = 1;

static struct device *g_registered_vcom_dev;

int device_init(struct device *dev)
{
    if (dev == NULL) {
        return -EINVAL;
    }
    INIT_LIST_HEAD(&dev->list);
    return 0;
}

int device_register(struct device *dev)
{
    if (dev != NULL) {
        dev->ready = 1U;
    }
    g_registered_vcom_dev = dev;
    return 0;
}

int device_is_ready(const struct device *dev)
{
    return (dev != NULL && dev->ready != 0U) ? 1 : 0;
}

struct nos_tty *nos_console_tty(void)
{
    return NULL;
}

int pr_log(bool no_tag, enum log_level level, const char *fmt, ...)
{
    (void)no_tag;
    (void)level;
    (void)fmt;
    return 0;
}

/* --------------------------------------------------------------------------
 * 模拟中断、内存、时钟与串口 console 桩
 * -------------------------------------------------------------------------- */
static addr_t g_irq_state;
static bool g_in_completion_hook;

addr_t disable_irq_save(void)
{
    addr_t prev = g_irq_state;
    g_irq_state = 1;
    return prev;
}

void enable_irq_save(addr_t flags)
{
    g_irq_state = flags;
}

void *kmalloc(size_t size, gfp_t flags)
{
    (void)flags;
    return malloc(size);
}

void *kzalloc(size_t size, gfp_t flags)
{
    (void)flags;
    return calloc(1, size);
}

void kfree(void *ptr)
{
    free(ptr);
}

uint64_t cpu_run_time_us(void)
{
    return 1000ULL;
}

void msleep(u32 msec)
{
    (void)msec;
}

static char g_uart_console_buf[1024];
static unsigned int g_uart_console_len;
static bool g_panic_mode;

int console_write_room(void)
{
    return 1024;
}

bool console_is_panic(void)
{
    return g_panic_mode;
}

int console_write(const char *buf, int len)
{
    if (len <= 0) {
        return 0;
    }
    if (g_uart_console_len + (unsigned int)len > sizeof(g_uart_console_buf)) {
        return -EAGAIN;
    }
    memcpy(g_uart_console_buf + g_uart_console_len, buf, (size_t)len);
    g_uart_console_len += (unsigned int)len;
    return len;
}

void console_send_log(void)
{
}

/* --------------------------------------------------------------------------
 * 模拟 completion（检测禁止在 log 锁内调用，并用 sigjmp 单线程推进 vcom_tx）
 * -------------------------------------------------------------------------- */
static bool g_vcom_log_calling_complete;
static sigjmp_buf g_tx_yield_jmp;
static bool g_tx_yield_active;
static unsigned int g_tx_wait_budget;
static int g_tx_wait_timeout_ret = -ETIMEDOUT;
static bool (*g_wait_ep_in_hook)(void);

int nos_completion_init(struct nos_completion *comp)
{
    if (comp == NULL) {
        return -EINVAL;
    }
    comp->done = 0;
    comp->closed = false;
    return 0;
}

int nos_complete(struct nos_completion *comp)
{
    if (g_in_completion_hook) {
        g_vcom_log_calling_complete = true;
    }
    if (comp == NULL) {
        return -EINVAL;
    }
    if (comp->done < UINT32_MAX - 1U) {
        comp->done++;
    }
    return 0;
}

int nos_complete_all(struct nos_completion *comp)
{
    if (comp == NULL) {
        return -EINVAL;
    }
    comp->done = UINT32_MAX;
    return 0;
}

int nos_completion_reset(struct nos_completion *comp)
{
    if (comp == NULL) {
        return -EINVAL;
    }
    comp->done = 0;
    return 0;
}

int nos_completion_wait(struct nos_completion *comp, nos_timeout_t timeout)
{
    if (comp == NULL) {
        return -EINVAL;
    }
    if (comp->done == UINT32_MAX) {
        return 0;
    }
    if (comp->done > 0U) {
        comp->done--;
        return 0;
    }
    if (timeout == NOS_NO_WAIT) {
        return -EAGAIN;
    }
    if (g_tx_yield_active) {
        if (timeout == NOS_WAIT_FOREVER && g_wait_ep_in_hook != NULL && g_wait_ep_in_hook()) {
            if (comp->done == UINT32_MAX) {
                return 0;
            }
            if (comp->done > 0U) {
                comp->done--;
                return 0;
            }
        }
        if (timeout != NOS_WAIT_FOREVER && g_tx_wait_budget > 0U) {
            g_tx_wait_budget--;
            return g_tx_wait_timeout_ret;
        }
        siglongjmp(g_tx_yield_jmp, 1);
    }
    return -ETIMEDOUT;
}

/* --------------------------------------------------------------------------
 * 模拟 SYS_INIT 注册与任务创建（记录并禁止保留旧的 vcom_rx_echo 任务）
 * -------------------------------------------------------------------------- */
typedef int (*test_sys_init_fn_t)(void);
static test_sys_init_fn_t g_sys_init_fns[8];
static unsigned int g_sys_init_count;

void test_register_sys_init(test_sys_init_fn_t fn)
{
    if (g_sys_init_count < ARRAY_SIZE(g_sys_init_fns)) {
        g_sys_init_fns[g_sys_init_count++] = fn;
    }
}

typedef void (*task_entry_fn_t)(void *param);
static struct task_struct g_created_tasks[8];
static task_entry_fn_t g_vcom_tx_entry;
static void *g_vcom_tx_param;
static unsigned int g_task_count;
static bool g_found_echo_task;

struct task_struct *task_create(const char *name,
                                void (*entry)(void *parameter),
                                void *parameter,
                                uint8_t priority,
                                uint32_t stack_size,
                                uint32_t tick,
                                void (*clean)(struct task_struct *task))
{
    (void)priority;
    (void)stack_size;
    (void)tick;
    (void)clean;

    assert(g_task_count < ARRAY_SIZE(g_created_tasks));
    if (name != NULL && strstr(name, "echo") != NULL) {
        g_found_echo_task = true;
    }
    g_created_tasks[g_task_count].name = name;
    if (entry != NULL && name != NULL && strcmp(name, "vcom") == 0) {
        g_vcom_tx_entry = entry;
        g_vcom_tx_param = parameter;
    }
    return &g_created_tasks[g_task_count++];
}

int task_ready(struct task_struct *task)
{
    assert(task != NULL);
    return 0;
}

/* --------------------------------------------------------------------------
 * 模拟 USB 设备核心层 API
 * -------------------------------------------------------------------------- */
static udclass_t g_registered_cdc_class;
static uint8_t g_ep0_tx_buf[64];
static size_t g_ep0_tx_size;
static int (*g_ep0_rx_cb)(udevice_t device, size_t size);
static void *g_ep0_rx_buf;
static size_t g_ep0_rx_size;

static struct uendpoint *g_ep_in;
static struct uendpoint *g_ep_out;
static struct uendpoint *g_ep_cmd;
static uint8_t g_usb_wire_tx[8192];
static size_t g_usb_wire_tx_len;
static size_t g_last_tx_pkt_size;
static unsigned int g_tx_pkt_count;
static unsigned int g_zlp_count;
static bool g_ep_in_busy;
static uint8_t g_ep_out_rx_buf[256];
static void *g_ep_out_armed_buf;
static size_t g_ep_out_armed_size;
static unsigned int g_ep_out_arm_count;

static void reset_usb_tx_capture(void)
{
    g_usb_wire_tx_len = 0;
    g_last_tx_pkt_size = 0;
    g_tx_pkt_count = 0;
    g_zlp_count = 0;
    g_ep_in_busy = false;
}

static void reset_usb_capture(void)
{
    reset_usb_tx_capture();
    g_ep_out_armed_buf = NULL;
    g_ep_out_armed_size = 0;
    g_ep_out_arm_count = 0;
}

int usbd_class_register(udclass_t udclass)
{
    g_registered_cdc_class = udclass;
    return 0;
}

ufunction_t usbd_function_new(udevice_t device, udev_desc_t dev_desc, ufunction_ops_t ops)
{
    ufunction_t func = calloc(1, sizeof(*func));
    assert(func != NULL);
    INIT_LIST_HEAD(&func->list);
    INIT_LIST_HEAD(&func->intf_list);
    func->device = device;
    func->dev_desc = dev_desc;
    func->ops = ops;
    return func;
}

uintf_t usbd_interface_new(udevice_t device, uintf_handler_t handler)
{
    uintf_t intf = calloc(1, sizeof(*intf));
    assert(intf != NULL);
    INIT_LIST_HEAD(&intf->list);
    INIT_LIST_HEAD(&intf->setting_list);
    intf->intf_num = device->nr_intf++;
    intf->handler = handler;
    return intf;
}

ualtsetting_t usbd_altsetting_new(size_t desc_size)
{
    ualtsetting_t setting = calloc(1, sizeof(*setting));
    assert(setting != NULL);
    setting->desc = calloc(1, desc_size > 0U ? desc_size : 1U);
    assert(setting->desc != NULL);
    setting->desc_size = desc_size;
    INIT_LIST_HEAD(&setting->list);
    INIT_LIST_HEAD(&setting->ep_list);
    return setting;
}

int usbd_altsetting_config_descriptor(ualtsetting_t setting, const void *desc, addr_t intf_pos)
{
    memcpy(setting->desc, desc, setting->desc_size);
    setting->intf_desc = (uintf_desc_t)((char *)setting->desc + intf_pos);
    return 0;
}

uep_t usbd_endpoint_new(uep_desc_t ep_desc, udep_handler_t handler)
{
    uep_t ep = calloc(1, sizeof(*ep));
    assert(ep != NULL);
    INIT_LIST_HEAD(&ep->list);
    INIT_LIST_HEAD(&ep->request_list);
    ep->ep_desc = ep_desc;
    ep->handler = handler;
    ep->buffer = NULL;
    if ((ep_desc->bEndpointAddress & USB_DIR_MASK) == USB_DIR_IN) {
        if ((ep_desc->bmAttributes & USB_EP_ATTR_TYPE_MASK) == USB_EP_ATTR_INT) {
            g_ep_cmd = ep;
        } else {
            g_ep_in = ep;
        }
    } else {
        g_ep_out = ep;
    }
    return ep;
}

int usbd_altsetting_add_endpoint(ualtsetting_t setting, uep_t ep)
{
    list_add_tail(&ep->list, &setting->ep_list);
    return 0;
}

int usbd_interface_add_altsetting(uintf_t intf, ualtsetting_t setting)
{
    list_add_tail(&setting->list, &intf->setting_list);
    return 0;
}

int usbd_set_altsetting(uintf_t intf, uint8_t value)
{
    (void)value;
    if (!list_empty(&intf->setting_list)) {
        intf->curr_setting = list_first_entry(&intf->setting_list, struct ualtsetting, list);
    }
    return 0;
}

int usbd_function_add_interface(ufunction_t func, uintf_t intf)
{
    list_add_tail(&intf->list, &func->intf_list);
    return 0;
}

int usbd_function_set_iad(ufunction_t func, uiad_desc_t iad_desc)
{
    (void)func;
    (void)iad_desc;
    return 0;
}

int usbd_device_set_descriptor(udevice_t device, udev_desc_t dev_desc)
{
    (void)device;
    (void)dev_desc;
    return 0;
}

int usbd_device_set_string(udevice_t device, const char **ustring)
{
    (void)device;
    (void)ustring;
    return 0;
}

int usbd_device_set_qualifier(udevice_t device, struct usb_qualifier_descriptor *qualifier)
{
    (void)device;
    (void)qualifier;
    return 0;
}

int usbd_os_comp_id_desc_add_os_func_comp_id_desc(usb_os_comp_id_desc_t os_comp_id_desc,
                                                  usb_os_func_comp_id_desc_t os_func_comp_id_desc)
{
    (void)os_comp_id_desc;
    (void)os_func_comp_id_desc;
    return 0;
}

size_t usbd_ep0_write(udevice_t device, void *buffer, size_t size)
{
    (void)device;
    assert(size <= sizeof(g_ep0_tx_buf));
    memcpy(g_ep0_tx_buf, buffer, size);
    g_ep0_tx_size = size;
    return size;
}

size_t usbd_ep0_read(udevice_t device, void *buffer, size_t size,
                     int (*rx_ind)(udevice_t device, size_t size))
{
    (void)device;
    g_ep0_rx_buf = buffer;
    g_ep0_rx_size = size;
    g_ep0_rx_cb = rx_ind;
    return size;
}

size_t usbd_io_request(udevice_t device, uep_t ep, uio_request_t req)
{
    (void)device;
    assert(ep != NULL);
    assert(req != NULL);

    if (ep == g_ep_in) {
        assert(!g_ep_in_busy);
        g_ep_in_busy = true;
        g_last_tx_pkt_size = req->size;
        g_tx_pkt_count++;
        if (req->size == 0U) {
            g_zlp_count++;
        } else {
            assert(g_usb_wire_tx_len + req->size <= sizeof(g_usb_wire_tx));
            memcpy(g_usb_wire_tx + g_usb_wire_tx_len, req->buffer, req->size);
            g_usb_wire_tx_len += req->size;
        }
        return req->size;
    }

    if (ep == g_ep_out) {
        g_ep_out_armed_buf = req->buffer;
        g_ep_out_armed_size = req->size;
        ep->buffer = req->buffer;
        g_ep_out_arm_count++;
        return req->size;
    }

    return 0;
}

static ufunction_t g_pump_func;

static bool complete_pending_ep_in(void)
{
    unsigned int spins = 0U;

    if (!g_ep_in_busy || g_pump_func == NULL) {
        return false;
    }
    while (g_ep_in_busy && spins < 32U) {
        size_t sent = g_last_tx_pkt_size;
        g_ep_in_busy = false;
        spins++;
        g_ep_in->handler(g_pump_func, sent);
    }
    assert(!g_ep_in_busy);
    return true;
}

static void pump_vcom_tx(ufunction_t func, unsigned int wait_budget)
{
    assert(g_vcom_tx_entry != NULL);
    g_pump_func = func;
    g_wait_ep_in_hook = complete_pending_ep_in;
    g_tx_wait_budget = wait_budget;
    g_tx_yield_active = true;
    if (sigsetjmp(g_tx_yield_jmp, 1) == 0) {
        g_vcom_tx_entry(g_vcom_tx_param);
    }
    g_tx_yield_active = false;
    g_wait_ep_in_hook = NULL;
    complete_pending_ep_in();
    assert(!g_ep_in_busy);
}

static int send_cdc_setup(ufunction_t func, uint8_t req_type, uint8_t bRequest,
                          uint16_t wValue, uint16_t wLength)
{
    struct urequest setup;
    uintf_t intf = list_first_entry(&func->intf_list, struct uinterface, list);

    memset(&setup, 0, sizeof(setup));
    setup.request_type = req_type;
    setup.bRequest = bRequest;
    setup.wValue = wValue;
    setup.wLength = wLength;
    return intf->handler(func, &setup);
}

static void inject_host_rx(ufunction_t func, const void *data, size_t len)
{
    assert(g_ep_out_armed_buf != NULL);
    assert(len <= g_ep_out_armed_size);
    memcpy(g_ep_out_armed_buf, data, len);
    g_ep_out_armed_buf = NULL;
    g_ep_out->handler(func, len);
}

static bool buf_contains(const uint8_t *haystack, size_t hlen, const char *needle)
{
    size_t nlen = strlen(needle);

    if (nlen == 0U || hlen < nlen) {
        return false;
    }
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (memcmp(haystack + i, needle, nlen) == 0) {
            return true;
        }
    }
    return false;
}

static int stub_ep0_send_status(void)
{
    return 0;
}

static const struct udcd_ops g_stub_dcd_ops = {
    .ep0_send_status = stub_ep0_send_status,
};

int main(void)
{
    struct udevice fake_dev;
    struct udcd fake_dcd;
    struct nos_tty *vcom_tty;
    ufunction_t func;
    struct ucdc_line_coding *lc;
    unsigned char rx_read_buf[128];
    uint32_t session_gen = 0U;
    bool non_interactive = false;
    int rc;

    memset(&fake_dev, 0, sizeof(fake_dev));
    memset(&fake_dcd, 0, sizeof(fake_dcd));
    fake_dcd.ops = &g_stub_dcd_ops;
    fake_dev.dcd = &fake_dcd;
    INIT_LIST_HEAD(&fake_dev.list);
    INIT_LIST_HEAD(&fake_dev.cfg_list);

    /* 1. 在注册与创建 USB CDC 功能前即可安全获取 TTY（初始化顺序无关） */
    kernel_log_init();
    vcom_tty = usbd_vcom_tty();
    assert(vcom_tty != NULL);
    assert(!nos_tty_poll_session(vcom_tty, &session_gen, &non_interactive));
    assert(usbd_vcom_class_register() == 0);
    assert(g_registered_cdc_class != NULL);

    /* 2. 创建 CDC 功能实例：验证日志后端自动切换、任务创建（无 echo 任务）与端点分配 */
    func = g_registered_cdc_class->usbd_function_create(&fake_dev);
    assert(func != NULL);
    assert(!g_found_echo_task);
    assert(g_task_count == 1U);
    assert(g_vcom_tx_entry != NULL);
    assert(g_ep_in != NULL && g_ep_out != NULL && g_ep_cmd != NULL);

    /* 写入大量连接前日志：确认不走 UART console、不调用 nos_complete 且溢出仅丢弃旧日志 */
    g_in_completion_hook = true;
    for (int i = 0; i < 40; i++) {
        char line[96];
        int n = snprintf(line, sizeof(line), "[boot-%02d] early vcom buffered log message\n", i);
        kernel_log_write(line, (unsigned int)n);
        kernel_log_flush();
    }
    g_in_completion_hook = false;
    assert(!g_vcom_log_calling_complete);
    assert(g_uart_console_len == 0U);

    /* Panic 模式仍应直通 UART 底层 console */
    g_panic_mode = true;
    kernel_log_write("PANIC!\n", 7U);
    g_panic_mode = false;
    assert(g_uart_console_len == 7U);

    /* 验证 GET_LINE_CODING 返回有效持久结构体且与默认值一致 */
    g_ep0_tx_size = 0;
    assert(send_cdc_setup(func, USB_REQ_TYPE_CLASS | USB_REQ_TYPE_INTERFACE | USB_DIR_IN,
                          CDC_GET_LINE_CODING, 0U, sizeof(*lc)) == 0);
    assert(g_ep0_tx_size == sizeof(*lc));
    lc = (struct ucdc_line_coding *)g_ep0_tx_buf;
    assert(lc->dwDTERate == 115200U && lc->bDataBits == 8U);

    /* 验证 SET_LINE_CODING 控制写入解析 */
    assert(send_cdc_setup(func, USB_REQ_TYPE_CLASS | USB_REQ_TYPE_INTERFACE | USB_DIR_OUT,
                          CDC_SET_LINE_CODING, 0U, sizeof(*lc)) == 0);
    assert(g_ep0_rx_cb != NULL && g_ep0_rx_buf != NULL && g_ep0_rx_size == sizeof(*lc));
    struct ucdc_line_coding new_lc = { 921600U, 0U, 0U, 8U };
    memcpy(g_ep0_rx_buf, &new_lc, sizeof(new_lc));
    assert(g_ep0_rx_cb(&fake_dev, sizeof(new_lc)) == 0);
    assert(send_cdc_setup(func, USB_REQ_TYPE_CLASS | USB_REQ_TYPE_INTERFACE | USB_DIR_IN,
                          CDC_GET_LINE_CODING, 0U, sizeof(*lc)) == 0);
    assert(((struct ucdc_line_coding *)g_ep0_tx_buf)->dwDTERate == 921600U);

    /* 3. 按 core.c 真实 SET_CONFIGURATION 顺序启用接口（ENABLE 时设备仍处于 ADDRESS 状态） */
    reset_usb_capture();
    fake_dev.state = USB_STATE_ADDRESS;
    assert(func->ops->enable(func) == 0);
    func->enabled = true;
    fake_dev.state = USB_STATE_CONFIGURED;
    assert(g_ep_out_arm_count == 1U);
    assert(!nos_tty_poll_session(vcom_tty, &session_gen, &non_interactive));

    /* 主机打开 DTR：轮询日志并把 pre-open 最新尾部日志发送给主机（含 CRLF 转换） */
    assert(send_cdc_setup(func, USB_REQ_TYPE_CLASS | USB_REQ_TYPE_INTERFACE | USB_DIR_OUT,
                          CDC_SET_CONTROL_LINE_STATE, 0x0001U, 0U) == 0);
    assert(nos_tty_poll_session(vcom_tty, &session_gen, &non_interactive));
    assert(!non_interactive);
    pump_vcom_tx(func, 1U);
    assert(g_usb_wire_tx_len > 0U);
    if (!buf_contains(g_usb_wire_tx, g_usb_wire_tx_len, "[boot-39] early vcom buffered log message\r\n")) {
        fprintf(stderr, "diag tx_len=%zu last200=[%.*s]\n",
                g_usb_wire_tx_len,
                (int)(g_usb_wire_tx_len > 200U ? 200U : g_usb_wire_tx_len),
                (const char *)(g_usb_wire_tx + (g_usb_wire_tx_len > 200U ? g_usb_wire_tx_len - 200U : 0U)));
    }
    assert(buf_contains(g_usb_wire_tx, g_usb_wire_tx_len, "[boot-39] early vcom buffered log message\r\n"));

    /* 4. 测试单包整除 64 字节触发 ZLP（Zero-Length Packet）收尾 */
    reset_usb_tx_capture();
    unsigned char pkt64[64];
    memset(pkt64, 'A', sizeof(pkt64));
    rc = nos_tty_write(vcom_tty, pkt64, sizeof(pkt64));
    assert(rc == 64);
    pump_vcom_tx(func, 0U);
    assert(g_usb_wire_tx_len == 64U);
    assert(g_zlp_count == 1U);

    /* 5. 测试主机注入 RX 数据不会触发任何自动回显，且仅由 nos_tty_read 消费 */
    reset_usb_tx_capture();
    inject_host_rx(func, "ps -w\r\n", 7U);
    assert(g_ep_out_arm_count == 2U);
    pump_vcom_tx(func, 0U);
    assert(g_usb_wire_tx_len == 0U);
    rc = nos_tty_read(vcom_tty, rx_read_buf, sizeof(rx_read_buf), false);
    assert(rc == 7);
    assert(memcmp(rx_read_buf, "ps -w\r\n", 7U) == 0);

    /* 6. 测试活动会话下 TX FIFO 满时：日志不覆盖/堵塞 TTY，ONLCR 整块退避不丢失半截换行 */
    unsigned char fill[128];
    memset(fill, 'X', sizeof(fill));
    int written_total = 0;
    for (;;) {
        rc = nos_tty_write(vcom_tty, fill, sizeof(fill));
        if (rc > 0) {
            written_total += rc;
            continue;
        }
        break;
    }
    assert(written_total >= 1024);

    /* 确认 TX FIFO 满时携带换行的小写请求直接返回 -EAGAIN 而非写入半个 CRLF */
    const unsigned char nl_msg[] = "a\nb";
    rc = nos_tty_write(vcom_tty, nl_msg, 3U);
    assert(rc == -EAGAIN);

    g_in_completion_hook = true;
    kernel_log_write("DROP_WHEN_ACTIVE_FULL\n", 22U);
    kernel_log_flush();
    g_in_completion_hook = false;
    assert(!g_vcom_log_calling_complete);

    /* 7. 测试 DTR 拉低与 USB 断开后会话态清理并允许干净重连 */
    pump_vcom_tx(func, 0U);
    assert(g_usb_wire_tx_len == (size_t)written_total);
    assert(!buf_contains(g_usb_wire_tx, g_usb_wire_tx_len, "DROP_WHEN_ACTIVE_FULL"));

    assert(send_cdc_setup(func, USB_REQ_TYPE_CLASS | USB_REQ_TYPE_INTERFACE | USB_DIR_OUT,
                          CDC_SET_CONTROL_LINE_STATE, 0x0000U, 0U) == 0);
    assert(!nos_tty_poll_session(vcom_tty, &session_gen, &non_interactive));

    /* 未发送 DTR 但直接发送数据时，rx_active 仍可激活轻量终端会话 */
    inject_host_rx(func, "\r", 1U);
    assert(nos_tty_poll_session(vcom_tty, &session_gen, &non_interactive));
    rc = nos_tty_read(vcom_tty, rx_read_buf, sizeof(rx_read_buf), false);
    assert(rc == 1 && rx_read_buf[0] == '\r');

    /* 8. 测试 RX FIFO 背压暂停与读取释放空间后自动重挂载 Bulk OUT */
    unsigned int prev_arm_count = g_ep_out_arm_count;
    uint8_t bulk_rx[64];
    memset(bulk_rx, 'R', sizeof(bulk_rx));
    while (g_ep_out_armed_buf != NULL) {
        inject_host_rx(func, bulk_rx, sizeof(bulk_rx));
    }
    /* FIFO 可用空间不足 64B 时暂停重挂载，形成硬件 NAK 背压 */
    assert(g_ep_out_arm_count > prev_arm_count);
    unsigned int paused_arm_count = g_ep_out_arm_count;
    while (nos_tty_read(vcom_tty, rx_read_buf, sizeof(rx_read_buf), false) > 0) {
    }
    /* 消费排空后必须自动重新挂载 Bulk OUT 端点 */
    assert(g_ep_out_arm_count == paused_arm_count + 1U);
    assert(g_ep_out_armed_buf != NULL);

    assert(func->ops->disable(func) == 0);
    func->enabled = false;
    fake_dev.state = USB_STATE_NOTATTACHED;
    assert(!nos_tty_poll_session(vcom_tty, &session_gen, &non_interactive));

    (void)g_ep_out_rx_buf;
    puts("usb cdc vcom log backend, tty bridge, and ep state machine checks passed");
    return 0;
}
