/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NUC472 片内 USBD 全速 (12 Mbps) USB 设备控制器驱动实现
 */
#include <kernel/kernel.h>
#include <kernel/types.h>
#include <kernel/errno.h>
#include <kernel/irq.h>
#include <kernel/printk.h>
#include <nos/hal_arch.h>
#include <drivers/usb/usb_common.h>
#include <drivers/usb/usb_device.h>
#include <soc/nuc472_soc.h>
#include <soc/nuc472_regs.h>
#include <soc/nuc472_periph.h>
#include <string.h>

#define NUC472_USBD_SLOT_NUM    10
#define NUC472_USBD_MPS         64

/* 硬件写 1 清除 (W1C) 辅助：真实芯片直接写值清除，宿主机单元测试对内存变量取反清除 */
#ifndef NUC472_HOST_TEST
#define nuc472_w1c_write(reg, val) do { (reg) = (val); } while (0)
#define nuc472_cep_read_byte()     ((uint8_t)NUC472_USBD->CEPDAT_BYTE)
#define nuc472_ep_read_byte(slot)  ((uint8_t)NUC472_USBD->EP[slot].EPDAT_BYTE)
#else
extern void nuc472_host_w1c_write(volatile uint32_t *reg, uint32_t val);
#define nuc472_w1c_write(reg, val) nuc472_host_w1c_write(&(reg), (val))
extern uint8_t nuc472_host_cep_read_byte(void);
extern uint8_t nuc472_host_ep_read_byte(int slot);
#define nuc472_cep_read_byte()     nuc472_host_cep_read_byte()
#define nuc472_ep_read_byte(slot)  nuc472_host_ep_read_byte(slot)
#endif

/* 硬件端点槽映射记录 */
struct nuc472_ep_slot {
    uint8_t ep_addr;       /* 逻辑端点地址，如 0x81 (IN), 0x01 (OUT)；0 表示未分配 */
    uint8_t type;          /* 端点属性类型：USB_EP_ATTR_BULK / USB_EP_ATTR_INT */
    bool in_use;           /* 是否已被逻辑端点占用使能 */
    bool is_in;            /* true 为 IN 端点，false 为 OUT 端点 */
    bool rx_pending;       /* 是否处于 OUT 准备接收状态 */
    bool rx_drained;       /* 标记最近一次 OUT 包已由 ISR 从 FIFO 搬空 */
    void *rx_buf;          /* 用户准备接收缓冲区 */
    size_t rx_buf_size;    /* 用户准备接收缓冲区大小 */
    size_t last_tx_len;    /* 最近一次 IN 发送字节数 */
    size_t last_rx_len;    /* 最近一次 OUT 接收字节数 */
};

static struct nuc472_ep_slot g_ep_slots[NUC472_USBD_SLOT_NUM];
static struct urequest s_setup_pkt;
static uint8_t s_pending_addr = 0;
static void *s_ep0_rx_buf = NULL;
static size_t s_ep0_rx_size = 0;

/* 前置声明 */
static int nuc472_usbd_init(void);

/* 查找逻辑端点绑定的硬件端点槽 */
static int nuc472_find_slot_by_addr(uint8_t address)
{
    for (int i = 0; i < NUC472_USBD_SLOT_NUM; i++) {
        if (g_ep_slots[i].in_use && g_ep_slots[i].ep_addr == address) {
            return i;
        }
    }
    return -1;
}

/* 为逻辑端点分配空闲的硬件端点槽 */
static int nuc472_alloc_slot(uint8_t address)
{
    int slot = nuc472_find_slot_by_addr(address);
    if (slot >= 0) {
        return slot;
    }
    for (int i = 0; i < NUC472_USBD_SLOT_NUM; i++) {
        if (!g_ep_slots[i].in_use) {
            return i;
        }
    }
    return -ENOMEM;
}

/* 设置设备地址（后置生效时序：等待状态阶段完成才写入硬件） */
static int nuc472_usbd_set_address(uint8_t address)
{
    addr_t flags = disable_irq_save();
    if (address == 0) {
        NUC472_USBD->FADDR = 0;
        s_pending_addr = 0;
    } else {
        s_pending_addr = address;
    }
    enable_irq_save(flags);
    return 0;
}

static int nuc472_usbd_set_config(__maybe_unused uint8_t address)
{
    return 0;
}

/**
 * @brief 安全向 EPRSPCTL 写入触发动作位，消除 RMW (|=) 误写 TOGGLE 或动作位隐患
 *
 * EPRSPCTL 寄存器中 Bit 3 (TOGGLE) 读出为当前 Data Toggle 状态 (DATA0/DATA1)，
 * 但写 1 会强制翻转/复位 Data Toggle。FLUSH、ZEROLEN、SHORTTXEN 也是写 1 触发动作。
 * 因此严禁使用 EPRSPCTL |= action。本函数保留当前的 MODE[2:1] 与 HALT[4] 状态，
 * 严格清零所有写 1 动作位后再或上本次指定的 action。
 */
static inline void nuc472_ep_trigger_action(int slot, uint32_t action)
{
    uint32_t ctl = NUC472_USBD->EP[slot].EPRSPCTL;
    ctl &= ~(NUC472_USBD_EPRSPCTL_FLUSH |
             NUC472_USBD_EPRSPCTL_TOGGLE |
             NUC472_USBD_EPRSPCTL_ZEROLEN |
             NUC472_USBD_EPRSPCTL_SHORTTXEN);
    NUC472_USBD->EP[slot].EPRSPCTL = ctl | action;
}

/* 端点 STALL 握手 */
static int nuc472_usbd_ep_set_stall(uint8_t address)
{
    addr_t flags = disable_irq_save();
    if (address == 0x00 || address == 0x80) {
        NUC472_USBD->CEPCTL |= NUC472_USBD_CEPCTL_STALLEN;
    } else {
        int slot = nuc472_find_slot_by_addr(address);
        if (slot >= 0) {
            nuc472_ep_trigger_action(slot, NUC472_USBD_EPRSPCTL_HALT);
        }
    }
    enable_irq_save(flags);
    return 0;
}

/* 端点 STALL 清除与 Toggle 复位 */
static int nuc472_usbd_ep_clear_stall(uint8_t address)
{
    addr_t flags = disable_irq_save();
    if (address == 0x00 || address == 0x80) {
        NUC472_USBD->CEPCTL &= ~NUC472_USBD_CEPCTL_STALLEN;
    } else {
        int slot = nuc472_find_slot_by_addr(address);
        if (slot >= 0) {
            uint32_t ctl = NUC472_USBD->EP[slot].EPRSPCTL;
            /* 清除 HALT[4] 以及所有写 1 动作位，保留 MODE[2:1]，写 1 到 TOGGLE 复位数据序列 */
            ctl &= ~(NUC472_USBD_EPRSPCTL_HALT |
                     NUC472_USBD_EPRSPCTL_FLUSH |
                     NUC472_USBD_EPRSPCTL_TOGGLE |
                     NUC472_USBD_EPRSPCTL_ZEROLEN |
                     NUC472_USBD_EPRSPCTL_SHORTTXEN);
            NUC472_USBD->EP[slot].EPRSPCTL = ctl | NUC472_USBD_EPRSPCTL_TOGGLE;
        }
    }
    enable_irq_save(flags);
    return 0;
}

/* 启用逻辑端点并分配硬件槽 */
static int nuc472_usbd_ep_enable(struct uendpoint *ep)
{
    if (ep == NULL || ep->ep_desc == NULL) {
        return -EINVAL;
    }

    uint8_t ep_addr = ep->ep_desc->bEndpointAddress;
    uint8_t ep_num = ep_addr & 0x0F;
    bool is_in = (ep_addr & 0x80) != 0;
    uint8_t attr = ep->ep_desc->bmAttributes & 0x03;

    addr_t flags = disable_irq_save();
    int slot = nuc472_alloc_slot(ep_addr);
    if (slot < 0) {
        enable_irq_save(flags);
        return -ENOMEM;
    }

    g_ep_slots[slot].in_use = true;
    g_ep_slots[slot].ep_addr = ep_addr;
    g_ep_slots[slot].is_in = is_in;
    g_ep_slots[slot].type = attr;
    g_ep_slots[slot].rx_pending = false;
    g_ep_slots[slot].rx_buf = NULL;
    g_ep_slots[slot].rx_buf_size = 0;
    g_ep_slots[slot].last_tx_len = 0;
    g_ep_slots[slot].last_rx_len = 0;

    /* 配置硬件 EPCFG 寄存器 */
    uint32_t epcfg = NUC472_USBD_EPCFG_EPEN | ((uint32_t)ep_num << NUC472_USBD_EPCFG_EPNUM_Pos);
    if (is_in) {
        epcfg |= NUC472_USBD_EPCFG_DIR_IN;
    }
    if (attr == USB_EP_ATTR_INT) {
        epcfg |= NUC472_USBD_EPCFG_TYPE_INT;
    } else {
        epcfg |= NUC472_USBD_EPCFG_TYPE_BULK;
    }

    NUC472_USBD->EP[slot].EPCFG = epcfg;
    NUC472_USBD->EP[slot].EPMPS = ep->ep_desc->wMaxPacketSize;

    /* 响应控制模式：Bulk 自动模式，INT 手动模式 */
    if (attr == USB_EP_ATTR_INT) {
        NUC472_USBD->EP[slot].EPRSPCTL = NUC472_USBD_EPRSPCTL_MODE_MANUAL | NUC472_USBD_EPRSPCTL_FLUSH;
    } else {
        NUC472_USBD->EP[slot].EPRSPCTL = NUC472_USBD_EPRSPCTL_MODE_AUTO | NUC472_USBD_EPRSPCTL_FLUSH;
    }

    /* 清除端点残留历史中断标志 (含借由 ERRIF 清除 SHORTRXIF)，防止使能后立即误触发 */
    NUC472_USBD->EP[slot].EPINTEN = 0;
    nuc472_w1c_write(NUC472_USBD->EP[slot].EPINTSTS,
                     NUC472_USBD_EPINT_TXPK | NUC472_USBD_EPINT_RXPK |
                     NUC472_USBD_EPINT_ERR | NUC472_USBD_EPINT_SHORTRX);

    /* 使能该端点的全局中断 */
    NUC472_USBD->GINTEN |= (1UL << (NUC472_USBD_GINTSTS_EP_SHIFT + slot));
    enable_irq_save(flags);

    return 0;
}

/* 禁用端点 */
static int nuc472_usbd_ep_disable(struct uendpoint *ep)
{
    if (ep == NULL || ep->ep_desc == NULL) {
        return -EINVAL;
    }

    uint8_t ep_addr = ep->ep_desc->bEndpointAddress;
    addr_t flags = disable_irq_save();
    int slot = nuc472_find_slot_by_addr(ep_addr);
    if (slot >= 0) {
        NUC472_USBD->EP[slot].EPCFG = 0;
        NUC472_USBD->EP[slot].EPINTEN = 0;
        NUC472_USBD->GINTEN &= ~(1UL << (NUC472_USBD_GINTSTS_EP_SHIFT + slot));
        memset(&g_ep_slots[slot], 0, sizeof(g_ep_slots[slot]));
    }
    enable_irq_save(flags);
    return 0;
}

/* OUT 接收准备 */
static size_t nuc472_usbd_ep_read_prepare(uint8_t address, void *buffer, size_t size)
{
    addr_t flags = disable_irq_save();
    if (address == 0x00) {
        s_ep0_rx_buf = buffer;
        s_ep0_rx_size = size;
        NUC472_USBD->CEPINTEN |= NUC472_USBD_CEPINT_RXPK;
        NUC472_USBD->CEPCTL = NUC472_USBD_CEPCTL_NAKCLR;
        enable_irq_save(flags);
        return size;
    }

    int slot = nuc472_find_slot_by_addr(address);
    if (slot >= 0) {
        g_ep_slots[slot].rx_pending = true;
        g_ep_slots[slot].rx_drained = false;
        g_ep_slots[slot].rx_buf = buffer;
        g_ep_slots[slot].rx_buf_size = size;
        /* 仅使能 RXPKIEN：NUC472 收到满包、短包或 0 字节包均会置位 RXPKIF；
         * 不使能 SHORTRXIEN，避免硬件特性下 SHORTRXIF 挂起时导致死循环重入 */
        NUC472_USBD->EP[slot].EPINTEN |= NUC472_USBD_EPINT_RXPK;
    }
    enable_irq_save(flags);
    return size;
}

/* 从硬件 OUT 端点 FIFO 搬运数据至缓冲区，并将超出部分彻底读出排空 */
static size_t nuc472_ep_drain_fifo(int slot, uint8_t *dst, size_t limit)
{
    uint32_t cnt = NUC472_USBD->EP[slot].EPDATCNT & 0xFFFF;
    size_t copy_len = (dst != NULL && limit > 0U) ? ((cnt < limit) ? cnt : limit) : 0U;

    for (size_t k = 0; k < copy_len; k++) {
        dst[k] = nuc472_ep_read_byte(slot);
    }
    /* 将超出接收缓冲区或未准备接收时的多余硬件 FIFO 字节读出丢弃，防止 FIFO 残留失步 */
    for (size_t k = copy_len; k < cnt; k++) {
        volatile uint8_t dummy = nuc472_ep_read_byte(slot);
        (void)dummy;
    }
    return copy_len;
}

/* 搬取端点 FIFO 数据 */
static size_t nuc472_usbd_ep_read(uint8_t address, void *buffer)
{
    if (buffer == NULL) {
        return 0;
    }

    addr_t flags = disable_irq_save();
    if (address == 0x00) {
        /* 控制端点：若为 SETUP 包请求读取则返回 8 字节 SETUP 数据 */
        memcpy(buffer, &s_setup_pkt, sizeof(struct urequest));
        enable_irq_save(flags);
        return sizeof(struct urequest);
    }

    int slot = nuc472_find_slot_by_addr(address);
    if (slot < 0) {
        enable_irq_save(flags);
        return 0;
    }

    /* 若 ISR 已将本次 OUT 数据搬运并排空 FIFO（包括 ZLP），core.c 触发回退读取时直接返回 0，避免误读后续新包 */
    if (g_ep_slots[slot].rx_drained) {
        g_ep_slots[slot].rx_drained = false;
        enable_irq_save(flags);
        return 0;
    }

    size_t limit = g_ep_slots[slot].rx_buf_size;
    if (limit == 0U) {
        limit = NUC472_USBD_MPS;
    }
    size_t copy_len = nuc472_ep_drain_fifo(slot, (uint8_t *)buffer, limit);
    g_ep_slots[slot].rx_pending = false;
    g_ep_slots[slot].last_rx_len = copy_len;
    g_ep_slots[slot].rx_buf = NULL;
    g_ep_slots[slot].rx_buf_size = 0;
    enable_irq_save(flags);
    return copy_len;
}

/* 向端点 FIFO 写入待发送数据 */
static size_t nuc472_usbd_ep_write(uint8_t address, void *buffer, size_t size)
{
    addr_t flags = disable_irq_save();
    if (address == 0x80) {
        /* 控制端点 EP0 IN */
        if (size == 0 || buffer == NULL) {
            NUC472_USBD->CEPCTL = NUC472_USBD_CEPCTL_ZEROLEN;
        } else {
            const uint8_t *src = (const uint8_t *)buffer;
            for (size_t k = 0; k < size; k++) {
                NUC472_USBD->CEPDAT_BYTE = src[k];
            }
            NUC472_USBD->CEPTXCNT = size;
        }
        NUC472_USBD->CEPINTEN |= (NUC472_USBD_CEPINT_TXPK | NUC472_USBD_CEPINT_STSDONE);
        enable_irq_save(flags);
        return size;
    }

    int slot = nuc472_find_slot_by_addr(address);
    if (slot < 0) {
        enable_irq_save(flags);
        return 0;
    }

    g_ep_slots[slot].last_tx_len = size;
    if (size == 0 || buffer == NULL) {
        nuc472_ep_trigger_action(slot, NUC472_USBD_EPRSPCTL_ZEROLEN);
    } else {
        const uint8_t *src = (const uint8_t *)buffer;
        for (size_t k = 0; k < size; k++) {
            NUC472_USBD->EP[slot].EPDAT_BYTE = src[k];
        }
        NUC472_USBD->EP[slot].EPTXCNT = size;

        uint32_t ep_mps = NUC472_USBD->EP[slot].EPMPS;
        if (ep_mps == 0) {
            ep_mps = NUC472_USBD_MPS;
        }

        /*
         * NUC472 硬件在 Bulk 端点（MODE_AUTO 自动模式）下：
         * 写入 FIFO 达到最大包长 (MPS) 时硬件自动触发发送；
         * 若发送 0 < size < ep_mps 的短包，硬件无法自动成包，必须置位 SHORTTXEN，
         * 否则硬件将对主机的 IN 令牌持续回 NAK 挂死。
         * 直接利用内存中记录的 g_ep_slots[slot].type == USB_EP_ATTR_BULK 判断，
         * 省去对 EPRSPCTL 的无意义 MMIO 读取（Minor #1）。
         * 对于 MODE_MANUAL 端点 (INT)，由软件直接写 EPTXCNT 触发，不需要置位 SHORTTXEN。
         */
        if (g_ep_slots[slot].type == USB_EP_ATTR_BULK && size < ep_mps) {
            nuc472_ep_trigger_action(slot, NUC472_USBD_EPRSPCTL_SHORTTXEN);
        }
    }
    NUC472_USBD->EP[slot].EPINTEN |= NUC472_USBD_EPINT_TXPK;
    enable_irq_save(flags);
    return size;
}

/* 控制传输 EP0 状态阶段发送 */
static int nuc472_usbd_ep0_send_status(void)
{
    addr_t flags = disable_irq_save();
    NUC472_USBD->CEPCTL = NUC472_USBD_CEPCTL_NAKCLR;
    enable_irq_save(flags);
    return 0;
}

static int nuc472_usbd_suspend(void)
{
    return 0;
}

static int nuc472_usbd_wakeup(void)
{
    addr_t flags = disable_irq_save();
    NUC472_USBD->OPER |= NUC472_USBD_OPER_RESUMEEN;
    enable_irq_save(flags);
    return 0;
}

/* udcd 操作结构体 */
static const struct udcd_ops _nuc472_udc_ops = {
    .set_address      = nuc472_usbd_set_address,
    .set_config       = nuc472_usbd_set_config,
    .ep_set_stall     = nuc472_usbd_ep_set_stall,
    .ep_clear_stall   = nuc472_usbd_ep_clear_stall,
    .ep_enable        = nuc472_usbd_ep_enable,
    .ep_disable       = nuc472_usbd_ep_disable,
    .ep_read_prepare  = nuc472_usbd_ep_read_prepare,
    .ep_read          = nuc472_usbd_ep_read,
    .ep_write         = nuc472_usbd_ep_write,
    .ep0_send_status  = nuc472_usbd_ep0_send_status,
    .suspend          = nuc472_usbd_suspend,
    .wakeup           = nuc472_usbd_wakeup,
};

/* 逻辑端点池（提供 EP0 + 10 路端点支持 WinUSB/CDC/HID 复合设备） */
static struct ep_id _nuc472_ep_pool[] = {
    {0x00, USB_EP_ATTR_CONTROL,   USB_DIR_INOUT, 64, ID_ASSIGNED  },
    {0x01, USB_EP_ATTR_BULK,      USB_DIR_IN,    64, ID_UNASSIGNED},
    {0x01, USB_EP_ATTR_BULK,      USB_DIR_OUT,   64, ID_UNASSIGNED},
    {0x02, USB_EP_ATTR_INT,       USB_DIR_IN,    64, ID_UNASSIGNED},
    {0x02, USB_EP_ATTR_INT,       USB_DIR_OUT,   64, ID_UNASSIGNED},
    {0x03, USB_EP_ATTR_BULK,      USB_DIR_IN,    64, ID_UNASSIGNED},
    {0x03, USB_EP_ATTR_BULK,      USB_DIR_OUT,   64, ID_UNASSIGNED},
    {0x04, USB_EP_ATTR_INT,       USB_DIR_IN,    64, ID_UNASSIGNED},
    {0x04, USB_EP_ATTR_INT,       USB_DIR_OUT,   64, ID_UNASSIGNED},
    {0x05, USB_EP_ATTR_BULK,      USB_DIR_IN,    64, ID_UNASSIGNED},
    {0x05, USB_EP_ATTR_BULK,      USB_DIR_OUT,   64, ID_UNASSIGNED},
    {0xFF, USB_EP_ATTR_TYPE_MASK, USB_DIR_MASK,   0, ID_ASSIGNED  },
};

/* 全局 udcd 控制器结构实例 */
struct udcd g_nuc472_udcd = {
    .init         = nuc472_usbd_init,
    .ops          = &_nuc472_udc_ops,
    .ep_pool      = _nuc472_ep_pool,
    .ep0          = { .id = &_nuc472_ep_pool[0] },
    .device_is_hs = 0, /* 严格锁定为全速 12 Mbps */
};

/* USBD 控制器底层初始化 */
static int nuc472_usbd_init(void)
{
    nuc472_sys_unlock();
    /* 使能片内 USB PHY 与 LDO33 稳压器，角色设为标准 Device (USBROLE[1:0] = 00b) */
    NUC472_SYS->USBPHY = (NUC472_SYS->USBPHY & ~NUC472_SYS_USBPHY_USBROLE_Msk) | NUC472_SYS_USBPHY_LDO33EN_Msk;
    /* 使能 USBD AHB 引擎时钟 */
    NUC472_CLK->AHBCLK |= NUC472_CLK_AHBCLK_USBDCKEN;
    /* 软复位 USBD 控制器 */
    NUC472_SYS->IPRST1 |= NUC472_SYS_IPRST1_USBDRST_Msk;
    NUC472_SYS->IPRST1 &= ~NUC472_SYS_IPRST1_USBDRST_Msk;
    nuc472_sys_lock();

    /*
     * 硬件时序要求：先保留 PHYCTL 出厂默认配置 (0x0420) 并仅置位 PHYEN 启动 UTMI PHY，
     * 通过写/读 EP[0].EPMPS 等待 UTMI 时钟稳定后再写其余 USBD 寄存器，避免写操作被静默丢弃。
     */
    NUC472_USBD->PHYCTL |= NUC472_USBD_PHYCTL_PHYEN;
    for (uint32_t wait = 0; wait < 100000U; wait++) {
        NUC472_USBD->EP[0].EPMPS = 0x20U;
        if (NUC472_USBD->EP[0].EPMPS == 0x20U) {
            break;
        }
    }

    /* 强制清零 HISPDEN 锁定为全速 (12 Mbps) */
    NUC472_USBD->OPER &= ~NUC472_USBD_OPER_HISPDEN;
    g_nuc472_udcd.device_is_hs = 0;

    /* CEP (EP0) 内部 SRAM 分区 [0x000, 0x03F] (64 字节) */
    NUC472_USBD->CEPBUFSTART = 0x000;
    NUC472_USBD->CEPBUFEND = 0x03F;

    /* 10 组硬件端点槽 (EPA ~ EPJ) 静态分配 64 字节互不重叠缓冲 (合计 704 字节 <= 4096 字节) */
    for (int i = 0; i < NUC472_USBD_SLOT_NUM; i++) {
        uint32_t start = 0x040 + i * 0x040;
        uint32_t end = start + 0x03F;
        NUC472_USBD->EP[i].EPBUFSTART = start;
        NUC472_USBD->EP[i].EPBUFEND = end;
        NUC472_USBD->EP[i].EPMPS = NUC472_USBD_MPS;
        NUC472_USBD->EP[i].EPCFG = 0;
        NUC472_USBD->EP[i].EPINTEN = 0;
        NUC472_USBD->EP[i].EPRSPCTL = NUC472_USBD_EPRSPCTL_FLUSH;
        memset(&g_ep_slots[i], 0, sizeof(g_ep_slots[i]));
    }

    s_pending_addr = 0;
    memset(&s_setup_pkt, 0, sizeof(s_setup_pkt));
    s_ep0_rx_buf = NULL;
    s_ep0_rx_size = 0;

    /* 清除并使能总线复位、挂起、恢复中断 */
    nuc472_w1c_write(NUC472_USBD->BUSINTSTS, 0x1FF);
    NUC472_USBD->BUSINTEN = NUC472_USBD_BUSINTEN_RSTIEN |
                            NUC472_USBD_BUSINTEN_RESUMEIEN |
                            NUC472_USBD_BUSINTEN_SUSPENDIEN;

    /* 清除并使能 CEP 控制端点中断 (SETUPPK, TXPK, RXPK, STSDONE) */
    nuc472_w1c_write(NUC472_USBD->CEPINTSTS, 0x1FFF);
    NUC472_USBD->CEPINTEN = NUC472_USBD_CEPINT_SETUPPK |
                            NUC472_USBD_CEPINT_TXPK |
                            NUC472_USBD_CEPINT_RXPK |
                            NUC472_USBD_CEPINT_STSDONE;

    /* 使能 USBD 全局总线与 CEP 中断 */
    NUC472_USBD->GINTEN = NUC472_USBD_GINTEN_USBIE | NUC472_USBD_GINTEN_CEPIE;

#ifndef NUC472_HOST_TEST
    hal_irq_enable(NUC472_USBD_IRQn);
#endif

    /* 控制器、端点及中断全部就绪后再使能 D+ 上拉电阻通知主机发起枚举 */
    NUC472_USBD->PHYCTL |= NUC472_USBD_PHYCTL_DPPUEN;

    return 0;
}

/* NUC472 USBD 硬件中断服务函数 (覆盖 irq_vectors.inc 中的弱符号) */
void USBD_IRQHandler(void)
{
    uint32_t gintsts = NUC472_USBD->GINTSTS;

    /* 1. 总线事件中断 (Bus Reset, Resume, Suspend) */
    if (gintsts & NUC472_USBD_GINTSTS_USBIF) {
        uint32_t busintsts = NUC472_USBD->BUSINTSTS;
        uint32_t unwanted_bus = busintsts & ~NUC472_USBD->BUSINTEN &
                                (NUC472_USBD_BUSINTSTS_PHYCLKVLDIF |
                                 NUC472_USBD_BUSINTSTS_VBUSDETIF);
        if (unwanted_bus != 0) {
            nuc472_w1c_write(NUC472_USBD->BUSINTSTS, unwanted_bus);
        }

        /* 总线复位事件 */
        if (busintsts & NUC472_USBD_BUSINTSTS_RSTIF) {
            nuc472_w1c_write(NUC472_USBD->BUSINTSTS, NUC472_USBD_BUSINTSTS_RSTIF);
            s_pending_addr = 0;
            s_ep0_rx_buf = NULL;
            s_ep0_rx_size = 0;
            NUC472_USBD->FADDR = 0;
            NUC472_USBD->CEPCTL = NUC472_USBD_CEPCTL_FLUSH;
            for (int i = 0; i < NUC472_USBD_SLOT_NUM; i++) {
                if (g_ep_slots[i].in_use) {
                    nuc472_ep_trigger_action(i, NUC472_USBD_EPRSPCTL_FLUSH);
                }
            }
            /* 保证复位后强制保持全速 */
            NUC472_USBD->OPER &= ~NUC472_USBD_OPER_HISPDEN;
            usbd_reset_handler(&g_nuc472_udcd);
        }

        /* 恢复连接事件 */
        if (busintsts & NUC472_USBD_BUSINTSTS_RESUMEIF) {
            nuc472_w1c_write(NUC472_USBD->BUSINTSTS, NUC472_USBD_BUSINTSTS_RESUMEIF);
            usbd_connect_handler(&g_nuc472_udcd);
        }

        /* 挂起断开事件 */
        if (busintsts & NUC472_USBD_BUSINTSTS_SUSPENDIF) {
            nuc472_w1c_write(NUC472_USBD->BUSINTSTS, NUC472_USBD_BUSINTSTS_SUSPENDIF);
            usbd_disconnect_handler(&g_nuc472_udcd);
        }
    }

    /* 2. CEP 控制端点中断 */
    if (gintsts & NUC472_USBD_GINTSTS_CEPIF) {
        uint32_t cepintsts = NUC472_USBD->CEPINTSTS;

        /* SETUP 包接收中断 */
        if (cepintsts & NUC472_USBD_CEPINT_SETUPPK) {
            s_setup_pkt.request_type = (uint8_t)(NUC472_USBD->SETUP1_0 & 0xFF);
            s_setup_pkt.bRequest = (uint8_t)((NUC472_USBD->SETUP1_0 >> 8) & 0xFF);
            s_setup_pkt.wValue = (uint16_t)(NUC472_USBD->SETUP3_2 & 0xFFFF);
            s_setup_pkt.wIndex = (uint16_t)(NUC472_USBD->SETUP5_4 & 0xFFFF);
            s_setup_pkt.wLength = (uint16_t)(NUC472_USBD->SETUP7_6 & 0xFFFF);

            nuc472_w1c_write(NUC472_USBD->CEPINTSTS, NUC472_USBD_CEPINT_SETUPPK);
            usbd_ep0_setup_handler(&g_nuc472_udcd, &s_setup_pkt);
        }

        /* EP0 IN 发送完成中断 */
        if (cepintsts & NUC472_USBD_CEPINT_TXPK) {
            nuc472_w1c_write(NUC472_USBD->CEPINTSTS, NUC472_USBD_CEPINT_TXPK);
            usbd_ep0_in_handler(&g_nuc472_udcd);
        }

        /* EP0 OUT 接收完成中断 */
        if (cepintsts & NUC472_USBD_CEPINT_RXPK) {
            uint32_t rx_cnt = NUC472_USBD->CEPRXCNT & 0xFFFF;
            if (rx_cnt == 0) {
                rx_cnt = NUC472_USBD->CEPDATCNT & 0xFFFF;
            }
            size_t copied = 0;
            if (s_ep0_rx_buf != NULL && s_ep0_rx_size > 0) {
                copied = (rx_cnt < s_ep0_rx_size) ? rx_cnt : s_ep0_rx_size;
                uint8_t *dst = (uint8_t *)s_ep0_rx_buf;
                for (size_t k = 0; k < copied; k++) {
                    dst[k] = nuc472_cep_read_byte();
                }
                /* 将超出缓冲区的多余字节从 FIFO 读出丢弃，防止 FIFO 残留导致硬件状态异常 */
                for (size_t k = copied; k < rx_cnt; k++) {
                    volatile uint8_t dummy = nuc472_cep_read_byte();
                    (void)dummy;
                }
                s_ep0_rx_buf = NULL;
                s_ep0_rx_size = 0;
            } else {
                /* 如果没有准备好的缓冲区，排空 FIFO */
                for (size_t k = 0; k < rx_cnt; k++) {
                    volatile uint8_t dummy = nuc472_cep_read_byte();
                    (void)dummy;
                }
            }
            /* 排空 CEP FIFO 后再清除 RXPK 标志，避免硬件因 FIFO 未空重触发 */
            nuc472_w1c_write(NUC472_USBD->CEPINTSTS, NUC472_USBD_CEPINT_RXPK);
            usbd_ep0_out_handler(&g_nuc472_udcd, copied);
        }

        /* 控制传输状态握手完成中断 (STSDONE) */
        if (cepintsts & NUC472_USBD_CEPINT_STSDONE) {
            nuc472_w1c_write(NUC472_USBD->CEPINTSTS, NUC472_USBD_CEPINT_STSDONE);
            /* 后置生效时序：此时将待生效地址写入硬件 FADDR */
            if (s_pending_addr != 0) {
                NUC472_USBD->FADDR = s_pending_addr;
                s_pending_addr = 0;
            }
            NUC472_USBD->CEPCTL = NUC472_USBD_CEPCTL_NAKCLR;
        }
    }

    /* 3. 业务端点 (EPA ~ EPJ, slot 0..9) 事件中断 */
    for (int i = 0; i < NUC472_USBD_SLOT_NUM; i++) {
        if (!(gintsts & (1UL << (NUC472_USBD_GINTSTS_EP_SHIFT + i)))) {
            continue;
        }
        uint32_t epintsts = NUC472_USBD->EP[i].EPINTSTS;

        /* IN 端点发送完成 */
        if (epintsts & NUC472_USBD_EPINT_TXPK) {
            nuc472_w1c_write(NUC472_USBD->EP[i].EPINTSTS, NUC472_USBD_EPINT_TXPK);
            NUC472_USBD->EP[i].EPINTEN &= ~NUC472_USBD_EPINT_TXPK;
            usbd_ep_in_handler(&g_nuc472_udcd, g_ep_slots[i].ep_addr, g_ep_slots[i].last_tx_len);
        }

        /* OUT 端点接收完成 (满包或短包到达) */
        if (!g_ep_slots[i].is_in &&
            (epintsts & NUC472_USBD->EP[i].EPINTEN &
             (NUC472_USBD_EPINT_RXPK | NUC472_USBD_EPINT_SHORTRX))) {
            uint32_t clear_mask = epintsts & (NUC472_USBD_EPINT_RXPK | NUC472_USBD_EPINT_SHORTRX);
            if ((clear_mask & NUC472_USBD_EPINT_SHORTRX) != 0U) {
                /* 实测 NUC472 硬件向 SHORTRXIF(Bit12) 写 1 无法清零，需一并置位 ERRIF(Bit11) 清除 */
                clear_mask |= NUC472_USBD_EPINT_ERR;
            }
            size_t limit = g_ep_slots[i].rx_buf_size;
            if (limit == 0U && g_ep_slots[i].rx_buf != NULL) {
                limit = NUC472_USBD_MPS;
            }
            uint8_t *dst = g_ep_slots[i].rx_pending ? (uint8_t *)g_ep_slots[i].rx_buf : NULL;
            size_t copied = nuc472_ep_drain_fifo(i, dst, limit);

            NUC472_USBD->EP[i].EPINTEN &= ~(NUC472_USBD_EPINT_RXPK | NUC472_USBD_EPINT_SHORTRX);
            g_ep_slots[i].rx_pending = false;
            g_ep_slots[i].rx_drained = true;
            g_ep_slots[i].rx_buf = NULL;
            g_ep_slots[i].rx_buf_size = 0;
            g_ep_slots[i].last_rx_len = copied;

            /* 在排空硬件 FIFO 之后再写 1 清除标志，防止 FIFO 未空时立刻重触发 SHORTRXIF */
            nuc472_w1c_write(NUC472_USBD->EP[i].EPINTSTS, clear_mask);
            usbd_ep_out_handler(&g_nuc472_udcd, g_ep_slots[i].ep_addr, copied);
        }
    }
}
