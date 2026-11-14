/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NUC472 片内 USBD 全速 (12 Mbps) 控制器驱动、端点调度与状态机主机单元测试
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define CONFIG_SYS_TICK_MS 1
#define __maybe_unused __attribute__((unused))
#ifndef min_t
#define min_t(type, a, b) (((type)(a) < (type)(b)) ? (type)(a) : (type)(b))
#endif
static inline void __maybe_unused pr_warning(const char *fmt, ...) { (void)fmt; }
static inline void __maybe_unused pr_info(const char *fmt, ...) { (void)fmt; }
static inline void __maybe_unused pr_err(const char *fmt, ...) { (void)fmt; }

#define NUC472_HOST_TEST 1
#include <soc/nuc472_regs.h>
#include <soc/nuc472_periph.h>
#include <soc/nuc472_soc.h>
#include <kernel/types.h>
#include <kernel/errno.h>
#include <kernel/irq.h>
#include <kernel/device.h>
#include <drivers/usb/usb_common.h>
#include <drivers/usb/usb_device.h>

/* 模拟硬件 USBD、SYS 与 CLK 寄存器 */
static NUC472_USBD_Type g_test_usbd;
static NUC472_SYS_Type g_test_sys;
static NUC472_CLK_Type g_test_clk;

NUC472_SYS_Type *nuc472_test_sys(void) { return &g_test_sys; }
NUC472_CLK_Type *nuc472_test_clk(void) { return &g_test_clk; }
NUC472_FMC_Type *nuc472_test_fmc(void) { return NULL; }
NUC472_RTC_Type *nuc472_test_rtc(void) { return NULL; }

#ifdef NUC472_USBD
#undef NUC472_USBD
#endif
#define NUC472_USBD (&g_test_usbd)

/* 模拟中断上下文与锁桩 */
static addr_t g_irq_flags = 0;
addr_t disable_irq_save(void)
{
    addr_t prev = g_irq_flags;
    g_irq_flags = 1;
    return prev;
}

void enable_irq_save(addr_t flags)
{
    g_irq_flags = flags;
}

void hal_irq_enable(uint32_t irq)
{
    (void)irq;
}

/* 记录并拦截 USBD core 回调事件 */
static int g_reset_count = 0;
static int g_connect_count = 0;
static int g_disconnect_count = 0;
static int g_setup_count = 0;
static struct urequest g_last_setup;
static int g_ep0_in_count = 0;
static int g_ep0_out_count = 0;
static size_t g_last_ep0_out_size = 0;
static int g_ep_in_count = 0;
static uint8_t g_last_ep_in_addr = 0;
static size_t g_last_ep_in_size = 0;
static int g_ep_out_count = 0;
static uint8_t g_last_ep_out_addr = 0;
static size_t g_last_ep_out_size = 0;

int usbd_reset_handler(udcd_t dcd)
{
    (void)dcd;
    g_reset_count++;
    return 0;
}

int usbd_connect_handler(udcd_t dcd)
{
    (void)dcd;
    g_connect_count++;
    return 0;
}

int usbd_disconnect_handler(udcd_t dcd)
{
    (void)dcd;
    g_disconnect_count++;
    return 0;
}

int usbd_ep0_setup_handler(udcd_t dcd, struct urequest *setup)
{
    (void)dcd;
    g_setup_count++;
    if (setup) {
        g_last_setup = *setup;
    }
    return 0;
}

int usbd_ep0_in_handler(udcd_t dcd)
{
    (void)dcd;
    g_ep0_in_count++;
    return 0;
}

int usbd_ep0_out_handler(udcd_t dcd, size_t size)
{
    (void)dcd;
    g_ep0_out_count++;
    g_last_ep0_out_size = size;
    return 0;
}

int usbd_ep_in_handler(udcd_t dcd, uint8_t address, size_t size)
{
    (void)dcd;
    g_ep_in_count++;
    g_last_ep_in_addr = address;
    g_last_ep_in_size = size;
    return 0;
}

int usbd_ep_out_handler(udcd_t dcd, uint8_t address, size_t size)
{
    (void)dcd;
    g_ep_out_count++;
    g_last_ep_out_addr = address;
    g_last_ep_out_size = size;
    return 0;
}

int usbd_sof_handler(udcd_t dcd)
{
    (void)dcd;
    return 0;
}

static inline void set_ro_reg(const volatile uint32_t *reg, uint32_t val)
{
    *(volatile uint32_t *)(uintptr_t)reg = val;
}

/* 包含驱动实现源文件（测试驱动内部逻辑与硬件交互） */
#include "../../drivers/usb/device/usbd_nuc472.c"

/* 测试 1: 控制器初始化、强制全速锁定、时钟使能与 SRAM 分区验证 */
static void test_usbd_init_and_speed_lock(void)
{
    memset(&g_test_usbd, 0, sizeof(g_test_usbd));
    memset(&g_test_sys, 0, sizeof(g_test_sys));
    memset(&g_test_clk, 0, sizeof(g_test_clk));

    /* 预设高速标志，验证 init 强制锁定清除全速 */
    g_test_usbd.OPER = NUC472_USBD_OPER_HISPDEN;

    int ret = g_nuc472_udcd.init();
    assert(ret == 0);

    /* 验证 LDO33 与 USB AHB 时钟使能 */
    assert((g_test_sys.USBPHY & NUC472_SYS_USBPHY_LDO33EN_Msk) != 0);
    assert((g_test_clk.AHBCLK & NUC472_CLK_AHBCLK_USBDCKEN) != 0);

    /* 验证强制全速 12 Mbps 锁定 (HISPDEN 清零且 device_is_hs == 0) */
    assert((g_test_usbd.OPER & NUC472_USBD_OPER_HISPDEN) == 0);
    assert(g_nuc472_udcd.device_is_hs == 0);

    /* 验证 CEP 内部 SRAM 缓冲分区 [0x000, 0x03F] (64 字节) */
    assert(g_test_usbd.CEPBUFSTART == 0x000);
    assert(g_test_usbd.CEPBUFEND == 0x03F);

    /* 验证至少 7 组端点 SRAM 缓冲互不重叠，每端点 64 字节且总占用 <= 4096 字节 */
    for (int i = 0; i < 7; i++) {
        uint32_t expected_start = 0x040 + i * 0x040;
        uint32_t expected_end = expected_start + 0x03F;
        assert(g_test_usbd.EP[i].EPBUFSTART == expected_start);
        assert(g_test_usbd.EP[i].EPBUFEND == expected_end);
        assert(g_test_usbd.EP[i].EPMPS == 64);
    }

    /* 验证 PHY 与上拉使能，中断使能 */
    assert((g_test_usbd.PHYCTL & NUC472_USBD_PHYCTL_PHYEN) != 0);
    assert((g_test_usbd.PHYCTL & NUC472_USBD_PHYCTL_DPPUEN) != 0);
    assert((g_test_usbd.BUSINTEN & NUC472_USBD_BUSINTEN_RSTIEN) != 0);
    assert((g_test_usbd.CEPINTEN & NUC472_USBD_CEPINT_SETUPPK) != 0);
    assert((g_test_usbd.GINTEN & NUC472_USBD_GINTEN_USBIE) != 0);
    assert((g_test_usbd.GINTEN & NUC472_USBD_GINTEN_CEPIE) != 0);

    printf("  [PASS] test_usbd_init_and_speed_lock\n");
}

/* 测试 2: 7 路逻辑端点启用与硬件槽分配验证 (Bulk 自动模式，INT 手动模式) */
static void test_ep_enable_and_slot_allocation(void)
{
    /* 模拟 7 个逻辑端点描述符 */
    struct uendpoint_descriptor ep_descs[7] = {
        { .bLength = sizeof(struct uendpoint_descriptor), .type = USB_DESC_TYPE_ENDPOINT,
          .bEndpointAddress = 0x81, .bmAttributes = USB_EP_ATTR_BULK, .wMaxPacketSize = 64 }, /* WinUSB IN */
        { .bLength = sizeof(struct uendpoint_descriptor), .type = USB_DESC_TYPE_ENDPOINT,
          .bEndpointAddress = 0x01, .bmAttributes = USB_EP_ATTR_BULK, .wMaxPacketSize = 64 }, /* WinUSB OUT */
        { .bLength = sizeof(struct uendpoint_descriptor), .type = USB_DESC_TYPE_ENDPOINT,
          .bEndpointAddress = 0x82, .bmAttributes = USB_EP_ATTR_INT,  .wMaxPacketSize = 64 }, /* CDC 通知 IN */
        { .bLength = sizeof(struct uendpoint_descriptor), .type = USB_DESC_TYPE_ENDPOINT,
          .bEndpointAddress = 0x83, .bmAttributes = USB_EP_ATTR_BULK, .wMaxPacketSize = 64 }, /* CDC 数据 IN */
        { .bLength = sizeof(struct uendpoint_descriptor), .type = USB_DESC_TYPE_ENDPOINT,
          .bEndpointAddress = 0x03, .bmAttributes = USB_EP_ATTR_BULK, .wMaxPacketSize = 64 }, /* CDC 数据 OUT */
        { .bLength = sizeof(struct uendpoint_descriptor), .type = USB_DESC_TYPE_ENDPOINT,
          .bEndpointAddress = 0x84, .bmAttributes = USB_EP_ATTR_INT,  .wMaxPacketSize = 64 }, /* HID IN */
        { .bLength = sizeof(struct uendpoint_descriptor), .type = USB_DESC_TYPE_ENDPOINT,
          .bEndpointAddress = 0x04, .bmAttributes = USB_EP_ATTR_INT,  .wMaxPacketSize = 64 }, /* HID OUT */
    };
    struct uendpoint eps[7];
    for (int i = 0; i < 7; i++) {
        memset(&eps[i], 0, sizeof(struct uendpoint));
        eps[i].ep_desc = &ep_descs[i];
        int ret = g_nuc472_udcd.ops->ep_enable(&eps[i]);
        assert(ret == 0);
    }

    /* 验证 7 路逻辑端点分配在 EPA..EPG (slot 0..6) 且配置正确 */
    for (int i = 0; i < 7; i++) {
        uint32_t epcfg = g_test_usbd.EP[i].EPCFG;
        assert((epcfg & NUC472_USBD_EPCFG_EPEN) != 0);

        uint8_t ep_num = ep_descs[i].bEndpointAddress & 0x0F;
        bool is_in = (ep_descs[i].bEndpointAddress & 0x80) != 0;
        assert(((epcfg >> NUC472_USBD_EPCFG_EPNUM_Pos) & 0x0F) == ep_num);
        if (is_in) {
            assert((epcfg & NUC472_USBD_EPCFG_DIR_IN) != 0);
        } else {
            assert((epcfg & NUC472_USBD_EPCFG_DIR_IN) == 0);
        }

        /* 验证响应控制模式：Bulk 自动模式，INT 手动模式 */
        if (ep_descs[i].bmAttributes == USB_EP_ATTR_INT) {
            assert((g_test_usbd.EP[i].EPRSPCTL & 0x02) == NUC472_USBD_EPRSPCTL_MODE_MANUAL);
        } else {
            assert((g_test_usbd.EP[i].EPRSPCTL & 0x02) == NUC472_USBD_EPRSPCTL_MODE_AUTO);
        }

        /* 验证全局端点中断使能位被开启 */
        assert((g_test_usbd.GINTEN & (1UL << (NUC472_USBD_GINTSTS_EP_SHIFT + i))) != 0);
    }

    printf("  [PASS] test_ep_enable_and_slot_allocation\n");
}

/* 测试 3: SETUPPKIF 捕获、8 字节组合与 ep_read(0x00) 准确读取 */
static void test_setup_packet_and_ep0(void)
{
    g_setup_count = 0;
    memset(&g_last_setup, 0, sizeof(g_last_setup));

    /* 模拟硬件收到 GET_DESCRIPTOR 设备描述符请求 (0x80, 0x06, 0x0100, 0x0000, 0x0040) */
    set_ro_reg(&g_test_usbd.SETUP1_0, 0x0680);
    set_ro_reg(&g_test_usbd.SETUP3_2, 0x0100);
    set_ro_reg(&g_test_usbd.SETUP5_4, 0x0000);
    set_ro_reg(&g_test_usbd.SETUP7_6, 0x0040);

    set_ro_reg(&g_test_usbd.GINTSTS, NUC472_USBD_GINTSTS_CEPIF);
    g_test_usbd.CEPINTSTS = NUC472_USBD_CEPINT_SETUPPK;

    USBD_IRQHandler();

    /* 验证 usbd_ep0_setup_handler 被回调且数据正确 */
    assert(g_setup_count == 1);
    assert(g_last_setup.request_type == 0x80);
    assert(g_last_setup.bRequest == 0x06);
    assert(g_last_setup.wValue == 0x0100);
    assert(g_last_setup.wIndex == 0x0000);
    assert(g_last_setup.wLength == 0x0040);

    /* 验证 SETUPPKIF 中断标志已清除 */
    assert((g_test_usbd.CEPINTSTS & NUC472_USBD_CEPINT_SETUPPK) == 0);

    /* 验证 ep_read(0x00, &setup) 读回完整 8 字节 */
    struct urequest read_req;
    memset(&read_req, 0, sizeof(read_req));
    size_t rd_sz = g_nuc472_udcd.ops->ep_read(0x00, &read_req);
    assert(rd_sz == sizeof(struct urequest));
    assert(memcmp(&read_req, &g_last_setup, sizeof(struct urequest)) == 0);

    printf("  [PASS] test_setup_packet_and_ep0\n");
}

/* 测试 4: SetAddress 后置生效时序与状态阶段 NAKCLR 闭环 */
static void test_set_address_deferred_timing(void)
{
    /* 设置地址为 0x1A (26) */
    g_test_usbd.FADDR = 0;
    g_nuc472_udcd.ops->set_address(0x1A);

    /* 在状态阶段到达前，硬件 FADDR 必须仍然保持旧地址 0，严禁提前生效 */
    assert(g_test_usbd.FADDR == 0);

    /* 发送 EP0 状态阶段 (ep0_send_status) 应当清除 NAK */
    g_nuc472_udcd.ops->ep0_send_status();
    assert((g_test_usbd.CEPCTL & NUC472_USBD_CEPCTL_NAKCLR) != 0);

    /* 模拟状态阶段握手完成中断 STSDONEIF */
    set_ro_reg(&g_test_usbd.GINTSTS, NUC472_USBD_GINTSTS_CEPIF);
    g_test_usbd.CEPINTSTS = NUC472_USBD_CEPINT_STSDONE;

    USBD_IRQHandler();

    /* 验证 STSDONEIF 到达后，新地址 0x1A 才写入硬件 FADDR 生效 */
    assert(g_test_usbd.FADDR == 0x1A);
    assert((g_test_usbd.CEPINTSTS & NUC472_USBD_CEPINT_STSDONE) == 0);

    printf("  [PASS] test_set_address_deferred_timing\n");
}

/* 测试 5: ep_write 数据发送、EPTXCNT 触发与 0 字节 ZLP 处理 */
static void test_ep_write_and_zlp(void)
{
    g_ep_in_count = 0;

    /* 1. EP0 ZLP 测试: ep_write(0x80, NULL, 0) 应置位 CEPCTL 的 ZEROLEN */
    g_test_usbd.CEPCTL = 0;
    size_t zlp_ret = g_nuc472_udcd.ops->ep_write(0x80, NULL, 0);
    assert(zlp_ret == 0);
    assert((g_test_usbd.CEPCTL & NUC472_USBD_CEPCTL_ZEROLEN) != 0);

    /* 2. 批量端点 0x81 (slot 0) 发送 64 字节数据 */
    uint8_t tx_data[64];
    for (int i = 0; i < 64; i++) {
        tx_data[i] = (uint8_t)(i + 0x20);
    }
    size_t wr_ret = g_nuc472_udcd.ops->ep_write(0x81, tx_data, 64);
    assert(wr_ret == 64);
    assert(g_test_usbd.EP[0].EPTXCNT == 64);
    assert((g_test_usbd.EP[0].EPINTEN & NUC472_USBD_EPINT_TXPK) != 0);

    /* 模拟硬件发送完成触发 TXPKIF 中断 */
    set_ro_reg(&g_test_usbd.GINTSTS, (1UL << (NUC472_USBD_GINTSTS_EP_SHIFT + 0)));
    g_test_usbd.EP[0].EPINTSTS = NUC472_USBD_EPINT_TXPK;

    USBD_IRQHandler();

    /* 验证回调 usbd_ep_in_handler 被触发且参数为 0x81, 64 */
    assert(g_ep_in_count == 1);
    assert(g_last_ep_in_addr == 0x81);
    assert(g_last_ep_in_size == 64);
    assert((g_test_usbd.EP[0].EPINTSTS & NUC472_USBD_EPINT_TXPK) == 0);

    printf("  [PASS] test_ep_write_and_zlp\n");
}

/* 测试 6: ep_read_prepare 接收准备、RXPKIF/SHORTRXIF 中断触发与 ep_read 搬空 FIFO */
static void test_ep_read_prepare_and_out(void)
{
    g_ep_out_count = 0;

    /* 针对 0x01 (OUT 端点，slot 1) 准备接收 64 字节 */
    uint8_t rx_buf[64];
    memset(rx_buf, 0, sizeof(rx_buf));
    size_t prep_ret = g_nuc472_udcd.ops->ep_read_prepare(0x01, rx_buf, 64);
    assert(prep_ret == 64);
    assert((g_test_usbd.EP[1].EPINTEN & (NUC472_USBD_EPINT_RXPK | NUC472_USBD_EPINT_SHORTRX)) != 0);

    /* 模拟硬件收到 32 字节短包并放入 EPDAT FIFO */
    set_ro_reg(&g_test_usbd.EP[1].EPDATCNT, 32);
    /* 模拟短包中断 SHORTRXIF */
    set_ro_reg(&g_test_usbd.GINTSTS, (1UL << (NUC472_USBD_GINTSTS_EP_SHIFT + 1)));
    g_test_usbd.EP[1].EPINTSTS = NUC472_USBD_EPINT_SHORTRX;

    USBD_IRQHandler();

    /* 验证 usbd_ep_out_handler 接收到 0x01, 32 */
    assert(g_ep_out_count == 1);
    assert(g_last_ep_out_addr == 0x01);
    assert(g_last_ep_out_size == 32);

    /* 验证随后 ep_read 准确读取并搬空 FIFO 计数 */
    size_t rd_ret = g_nuc472_udcd.ops->ep_read(0x01, rx_buf);
    assert(rd_ret == 32);

    printf("  [PASS] test_ep_read_prepare_and_out\n");
}

/* 测试 7: 总线复位 RSTIF 事件处理 (清除地址、Flush 端点与保持全速锁定) */
static void test_bus_reset_behavior(void)
{
    g_reset_count = 0;
    g_test_usbd.FADDR = 0x2A;
    g_test_usbd.OPER |= NUC472_USBD_OPER_HISPDEN; /* 恶意置位高速 */

    set_ro_reg(&g_test_usbd.GINTSTS, NUC472_USBD_GINTSTS_USBIF);
    g_test_usbd.BUSINTSTS = NUC472_USBD_BUSINTSTS_RSTIF;

    USBD_IRQHandler();

    assert(g_reset_count == 1);
    assert(g_test_usbd.FADDR == 0);
    assert((g_test_usbd.OPER & NUC472_USBD_OPER_HISPDEN) == 0); /* 必须强制保持全速 */
    assert((g_test_usbd.BUSINTSTS & NUC472_USBD_BUSINTSTS_RSTIF) == 0);

    printf("  [PASS] test_bus_reset_behavior\n");
}

int main(void)
{
    printf("=== NUC472 USBD Controller & Endpoint State Machine Tests ===\n");
    test_usbd_init_and_speed_lock();
    test_ep_enable_and_slot_allocation();
    test_setup_packet_and_ep0();
    test_set_address_deferred_timing();
    test_ep_write_and_zlp();
    test_ep_read_prepare_and_out();
    test_bus_reset_behavior();
    printf("All NUC472 USBD host tests passed successfully!\n");
    return 0;
}
